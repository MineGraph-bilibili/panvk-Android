/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 *
 * Derived from tu_wsi.c:
 * Copyright © 2016 Red Hat
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_wsi.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"

#include <stdlib.h>
#include <string.h>

#if defined(HAVE_PAN_KMOD_KBASE)
#include "lib/kmod/kbase_kmod.h"
#endif

#include "vk_util.h"
#include "wsi_common.h"

static VKAPI_PTR PFN_vkVoidFunction
panvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);
   struct panvk_instance *instance = to_panvk_instance(pdevice->vk.instance);

   return vk_instance_get_proc_addr_unchecked(&instance->vk, pName);
}

static bool
panvk_can_present_on_device(VkPhysicalDevice pdevice, int fd)
{
   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0)
      return false;
   /* Allow on-device presentation for all devices with bus type PLATFORM.
    * Other device types such as PCI or USB should use the PRIME blit path. */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include "wsi_common_private.h"
#include "panvk_android.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_image.h"
#include "util/log.h"
#include "vk_object.h"
#include "vk_android.h"

struct panvk_android_swapchain_image {
   struct wsi_image base;
   AHardwareBuffer *ahb;
   uint32_t width;
   uint32_t height;
   uint32_t stride; /* in pixels */
   /* Zero-copy only: the window buffer backing this image, and whether that
    * buffer is currently dequeued from the window (the app may render into
    * it). Both are unused by the copy path. */
   void *anb;
   bool held;
};

struct panvk_android_swapchain {
   struct wsi_swapchain base;
   ANativeWindow *window;
   uint32_t next_image;
   uint64_t completed_present_id;
   /* Zero-copy: the images wrap the window's own buffers, present only
    * queues them back. */
   bool zero_copy;
   struct panvk_android_swapchain_image images[4];
};

/* ------------------------------------------------------------------------- *
 * Zero-copy present
 *
 * The copy path renders into a driver-owned AHardwareBuffer and then memcpy's
 * the whole frame into the window's next buffer inside ANativeWindow_lock().
 * That is a full-frame CPU copy (plus a cache-cold read of the AHB) on every
 * present, and the frame reaches the compositor whenever the CPU gets around
 * to it.
 *
 * libnativewindow exports the producer-side BufferQueue entry points as LLNDK
 * symbols (ANativeWindow_dequeueBuffer / queueBuffer / cancelBuffer,
 * ANativeWindowBuffer_getHardwareBuffer, ANativeWindow_setBufferCount), but
 * the NDK does not declare them, so they are resolved with dlsym. With them
 * the swapchain imports the window's own buffers as VkImages and the GPU
 * renders straight into the surface: present becomes a single queueBuffer()
 * and the compositor owns pacing (no CPU copy, no tearing).
 *
 * This is opportunistic. If the symbols are missing, the window refuses to
 * hand out buffers, a buffer's geometry/format cannot be used as-is, the
 * dma-buf cannot be imported (AFBC gralloc buffers are rejected by kbase on
 * some vendors) or the row pitch panvk derives disagrees with gralloc's
 * stride, swapchain creation falls back to the copy path. Setting
 * PANVK_WSI_ZERO_COPY=0 forces the copy path.
 * ------------------------------------------------------------------------- */

struct panvk_android_anw_ops {
   int32_t (*dequeue_buffer)(ANativeWindow *window, void **buffer, int *fence_fd);
   int32_t (*queue_buffer)(ANativeWindow *window, void *buffer, int fence_fd);
   int32_t (*cancel_buffer)(ANativeWindow *window, void *buffer, int fence_fd);
   AHardwareBuffer *(*buffer_to_ahb)(void *buffer);
   int32_t (*set_buffer_count)(ANativeWindow *window, size_t buffer_count);
   int32_t (*query)(const ANativeWindow *window, int query, int *value);
};

static struct panvk_android_anw_ops panvk_android_anw;
static pthread_once_t panvk_android_anw_once = PTHREAD_ONCE_INIT;

static void *
panvk_android_anw_sym(void *lib, const char *name)
{
   void *sym = lib ? dlsym(lib, name) : NULL;

   return sym ? sym : dlsym(RTLD_DEFAULT, name);
}

static void
panvk_android_anw_resolve(void)
{
   /* libnativewindow is a public (LLNDK) library, so the app namespace can
    * load it and every wrapper above is visible. */
   void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);

   *(void **)&panvk_android_anw.dequeue_buffer =
      panvk_android_anw_sym(lib, "ANativeWindow_dequeueBuffer");
   *(void **)&panvk_android_anw.queue_buffer =
      panvk_android_anw_sym(lib, "ANativeWindow_queueBuffer");
   *(void **)&panvk_android_anw.cancel_buffer =
      panvk_android_anw_sym(lib, "ANativeWindow_cancelBuffer");
   *(void **)&panvk_android_anw.buffer_to_ahb =
      panvk_android_anw_sym(lib, "ANativeWindowBuffer_getHardwareBuffer");
   *(void **)&panvk_android_anw.set_buffer_count =
      panvk_android_anw_sym(lib, "ANativeWindow_setBufferCount");
   *(void **)&panvk_android_anw.query =
      panvk_android_anw_sym(lib, "ANativeWindow_query");
}

static void
panvk_android_anw_wait_fence(int fence_fd)
{
   if (fence_fd < 0)
      return;

   /* The fence only says when the previous owner stopped touching the buffer;
    * wait for it, but never let a wedged consumer hang acquire forever. */
   struct pollfd pfd = {.fd = fence_fd, .events = POLLIN};
   poll(&pfd, 1, 1000);
   close(fence_fd);
}

static bool
panvk_android_zero_copy_allowed(void)
{
   const char *env = getenv("PANVK_WSI_ZERO_COPY");

   return !env || strcmp(env, "0") != 0;
}

static bool
panvk_android_zero_copy_format_matches(uint32_t ahb_format, VkFormat vk_format)
{
   switch (ahb_format) {
   case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
   case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      return vk_format == VK_FORMAT_R8G8B8A8_UNORM ||
             vk_format == VK_FORMAT_R8G8B8A8_SRGB;
   /* AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM == 5. The Android stub header
    * shipped with the Mesa build has no name for it. */
   case 5:
      return vk_format == VK_FORMAT_B8G8R8A8_UNORM ||
             vk_format == VK_FORMAT_B8G8R8A8_SRGB;
   default:
      return false;
   }
}

static int
panvk_android_swapchain_find_anb(struct panvk_android_swapchain *chain,
                                 void *anb)
{
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].anb == anb)
         return (int)i;
   }

   return -1;
}

/* ANativeWindowQuery value from vndk/window.h; the NDK does not expose it. */
#define PANVK_ANDROID_QUERY_MIN_UNDEQUEUED_BUFFERS 3

/* Import an AHardwareBuffer as one swapchain image. The image is created
 * through the same deferred Android path the copy path uses, so panvk reads
 * the buffer's real DRM modifier and plane layout from gralloc when the
 * memory is imported. That makes window buffers usable as render targets
 * whatever the vendor hands out (linear or AFBC). */
static VkResult
panvk_android_swapchain_import_ahb(struct panvk_android_swapchain *chain,
                                   VkDevice device,
                                   const VkAllocationCallbacks *pAllocator,
                                   uint32_t index,
                                   struct AHardwareBuffer *ahb,
                                   const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   VK_FROM_HANDLE(vk_device, dev, device);
   struct panvk_android_swapchain_image *img = &chain->images[index];
   VkResult result;

   VkExternalMemoryImageCreateInfo emici = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &emici,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->imageFormat,
      .extent = {pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = pCreateInfo->imageUsage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = dev->dispatch_table.CreateImage(device, &ici, pAllocator,
                                            &img->base.image);
   if (result != VK_SUCCESS)
      return result;

   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
   };
   result = panvk_GetAndroidHardwareBufferPropertiesANDROID(device, ahb,
                                                            &ahb_props);
   if (result != VK_SUCCESS)
      return result;

   uint32_t chosen_mi = 0;
   for (uint32_t m = 0; m < 32; m++) {
      if (ahb_props.memoryTypeBits & (1u << m)) {
         chosen_mi = m;
         break;
      }
   }

   VkImportAndroidHardwareBufferInfoANDROID imp = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .buffer = ahb,
   };
   VkMemoryDedicatedAllocateInfo dai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &imp,
      .image = img->base.image,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dai,
      .allocationSize = ahb_props.allocationSize,
      .memoryTypeIndex = chosen_mi,
   };
   result = dev->dispatch_table.AllocateMemory(device, &mai, pAllocator,
                                               &img->base.memory);
   if (result != VK_SUCCESS)
      return result;

   VkBindImageMemoryInfo bmi = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = img->base.image,
      .memory = img->base.memory,
      .memoryOffset = 0,
   };
   return dev->dispatch_table.BindImageMemory2(device, 1, &bmi);
}

/* Copy path: each image owns a driver-allocated AHB and present() memcpy's the
 * frame into the window's buffer under ANativeWindow_lock(). */
static VkResult
panvk_android_swapchain_init_copy(struct panvk_android_swapchain *chain,
                                  VkDevice device,
                                  const VkAllocationCallbacks *pAllocator,
                                  const VkSwapchainCreateInfoKHR *pCreateInfo,
                                  uint32_t num_images)
{
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < num_images; i++) {
      AHardwareBuffer_Desc desc = {
         .width = pCreateInfo->imageExtent.width,
         .height = pCreateInfo->imageExtent.height,
         .layers = 1,
         .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
         .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
      };
      if (AHardwareBuffer_allocate(&desc, &chain->images[i].ahb) != 0 ||
          !chain->images[i].ahb)
         return VK_ERROR_INITIALIZATION_FAILED;

      /* Ours and still undefined: the no-mapper linear probe may write it. */
      vk_android_set_owned_ahb(chain->images[i].ahb);

      /* Cache the geometry so present does not query the mapper every frame. */
      AHardwareBuffer_Desc img_desc;
      AHardwareBuffer_describe(chain->images[i].ahb, &img_desc);
      chain->images[i].width = img_desc.width;
      chain->images[i].height = img_desc.height;
      chain->images[i].stride = img_desc.stride;

      result = panvk_android_swapchain_import_ahb(chain, device, pAllocator, i,
                                                  chain->images[i].ahb,
                                                  pCreateInfo);
      if (result != VK_SUCCESS)
         break;
   }

   vk_android_set_owned_ahb(NULL);
   return result;
}

/* Zero-copy: import the window's own buffers and let the GPU render into them.
 *
 * The image pool has to be sized before any buffer is held for the life of the
 * swapchain. Query M, the number of buffers that must stay un-dequeued, raise
 * the pool to image_count + M, then dequeue the image_count buffers this
 * swapchain needs and cancel them again. Before the first queued buffer the
 * dequeue limit does not apply, so this cannot block, and afterwards the pool
 * is large enough for the app to hold every image it may acquire. */
static VkResult
panvk_android_swapchain_init_zero_copy(struct panvk_android_swapchain *chain,
                                       VkDevice device,
                                       const VkAllocationCallbacks *pAllocator,
                                       const VkSwapchainCreateInfoKHR *pCreateInfo,
                                       uint32_t num_images,
                                       uint32_t *out_num_images)
{
   if (!chain->window || !panvk_android_anw.dequeue_buffer ||
       !panvk_android_anw.queue_buffer || !panvk_android_anw.cancel_buffer ||
       !panvk_android_anw.buffer_to_ahb || !panvk_android_anw.set_buffer_count ||
       !panvk_android_anw.query) {
      mesa_logi("panvk: zero-copy unavailable: ANativeWindow producer symbols "
                "not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int min_undequeued = 0;
   if (panvk_android_anw.query(chain->window,
                               PANVK_ANDROID_QUERY_MIN_UNDEQUEUED_BUFFERS,
                               &min_undequeued) != 0 ||
       min_undequeued < 0) {
      mesa_logi("panvk: zero-copy unavailable: ANativeWindow_query failed");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int32_t set_count_status =
      panvk_android_anw.set_buffer_count(chain->window,
                                         num_images + min_undequeued);
   if (set_count_status != 0) {
      mesa_logi("panvk: zero-copy unavailable: the window rejected a buffer "
                "count of %u (status %d)", num_images + min_undequeued,
                set_count_status);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   uint32_t count = 0;
   for (; count < num_images; count++) {
      void *anb = NULL;
      int fence_fd = -1;
      if (panvk_android_anw.dequeue_buffer(chain->window, &anb, &fence_fd) != 0 ||
          !anb) {
         if (fence_fd >= 0)
            close(fence_fd);
         mesa_logi("panvk: zero-copy unavailable: dequeue failed after %u "
                   "buffers", count);
         break;
      }

      AHardwareBuffer *ahb = panvk_android_anw.buffer_to_ahb(anb);
      if (!ahb) {
         panvk_android_anw.cancel_buffer(chain->window, anb, -1);
         close(fence_fd);
         break;
      }

      AHardwareBuffer_Desc desc;
      AHardwareBuffer_describe(ahb, &desc);
      if (desc.width != pCreateInfo->imageExtent.width ||
          desc.height != pCreateInfo->imageExtent.height ||
          desc.layers != 1 ||
          !panvk_android_zero_copy_format_matches(desc.format,
                                                 pCreateInfo->imageFormat)) {
         mesa_logi("panvk: window buffer %ux%ux%u fmt 0x%x cannot back a %ux%u "
                   "VkFormat %d swapchain image", desc.width, desc.height,
                   desc.layers, desc.format, pCreateInfo->imageExtent.width,
                   pCreateInfo->imageExtent.height, pCreateInfo->imageFormat);
         panvk_android_anw.cancel_buffer(chain->window, anb, -1);
         close(fence_fd);
         break;
      }

      panvk_android_anw_wait_fence(fence_fd);

      chain->images[count].anb = anb;
      VkResult import_result =
         panvk_android_swapchain_import_ahb(chain, device, pAllocator, count,
                                            ahb, pCreateInfo);
      if (import_result != VK_SUCCESS) {
         chain->images[count].anb = NULL;
         panvk_android_anw.cancel_buffer(chain->window, anb, -1);
         mesa_logi("panvk: zero-copy unavailable: dma-buf import of a %ux%u "
                   "format 0x%x window buffer failed (%d)", desc.width,
                   desc.height, desc.format, import_result);
         break;
      }
   }

   /* Every buffer goes back to the window; from now on acquire dequeues one
    * and present queues it. */
   for (uint32_t i = 0; i < count; i++)
      panvk_android_anw.cancel_buffer(chain->window, chain->images[i].anb, -1);

   if (count < 2 || count < pCreateInfo->minImageCount) {
      mesa_logi("panvk: zero-copy unavailable: the window handed out only %u "
                "usable buffers", count);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   *out_num_images = count;
   return VK_SUCCESS;
}

static void
panvk_android_swapchain_free_images(struct panvk_android_swapchain *chain,
                                    VkDevice device,
                                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_device, dev, device);

   for (uint32_t i = 0; i < ARRAY_SIZE(chain->images); i++) {
      struct panvk_android_swapchain_image *img = &chain->images[i];

      if (chain->zero_copy && img->held && img->anb && chain->window) {
         panvk_android_anw.cancel_buffer(chain->window, img->anb, -1);
         img->held = false;
      }

      if (img->base.image != VK_NULL_HANDLE)
         dev->dispatch_table.DestroyImage(device, img->base.image, pAllocator);
      if (img->base.memory != VK_NULL_HANDLE)
         dev->dispatch_table.FreeMemory(device, img->base.memory, pAllocator);
      /* Copy path only: zero-copy images are backed by buffers the window owns
       * and the imported memory already holds the buffer reference. */
      if (img->ahb)
         AHardwareBuffer_release(img->ahb);

      img->ahb = NULL;
      img->anb = NULL;
      img->base.image = VK_NULL_HANDLE;
      img->base.memory = VK_NULL_HANDLE;
   }
}

static VkResult
panvk_wsi_android_get_support(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t queueFamilyIndex,
                              VkBool32 *pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_get_capabilities2(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    const void *info_next,
                                    VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   VkIcdSurfaceAndroid *s = (VkIcdSurfaceAndroid *)surface;
   int w = s->window ? ANativeWindow_getWidth(s->window) : 64;
   int h = s->window ? ANativeWindow_getHeight(s->window) : 64;
   if (w <= 0) w = 64;
   if (h <= 0) h = 64;

   pSurfaceCapabilities->surfaceCapabilities = (VkSurfaceCapabilitiesKHR){
      .minImageCount = 2,
      .maxImageCount = 4,
      .currentExtent = {(uint32_t)w, (uint32_t)h},
      .minImageExtent = {1, 1},
      .maxImageExtent = {4096, 4096},
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT,
   };

   const VkSurfacePresentModeKHR *mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);
   vk_foreach_struct(sType, ext, pSurfaceCapabilities->pNext) {
      if (sType == VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR) {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         const VkPresentModeKHR present_mode =
            mode ? mode->presentMode : VK_PRESENT_MODE_FIFO_KHR;
         VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out,
                                compat->pPresentModes,
                                &compat->presentModeCount);
         vk_outarray_append_typed(VkPresentModeKHR, &out, compatible_mode) {
            *compatible_mode = present_mode;
         }
      }
   }
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_get_formats(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t *pSurfaceFormatCount,
                              VkSurfaceFormatKHR *pSurfaceFormats)
{
   static const VkSurfaceFormatKHR formats[] = {
      { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
      { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
      { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   };
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         *f = formats[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_formats2(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               const void *info_next,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormat2KHR *pSurfaceFormats)
{
   static const VkSurfaceFormatKHR formats[] = {
      { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
      { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
      { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   };
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat = formats[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_present_modes(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    uint32_t *pPresentModeCount,
                                    VkPresentModeKHR *pPresentModes)
{
   static const VkPresentModeKHR modes[] = {
      VK_PRESENT_MODE_FIFO_KHR,
      VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_IMMEDIATE_KHR,
   };
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes, pPresentModeCount);
   for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
      vk_outarray_append_typed(VkPresentModeKHR, &out, m) {
         *m = modes[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_present_rectangles(VkIcdSurfaceBase *surface,
                                         struct wsi_device *wsi_device,
                                         uint32_t *pRectCount,
                                         VkRect2D *pRects)
{
   VkIcdSurfaceAndroid *s = (VkIcdSurfaceAndroid *)surface;
   int w = s->window ? ANativeWindow_getWidth(s->window) : 64;
   int h = s->window ? ANativeWindow_getHeight(s->window) : 64;
   if (w <= 0) w = 64;
   if (h <= 0) h = 64;

   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, r) {
      r->offset.x = 0;
      r->offset.y = 0;
      r->extent.width = (uint32_t)w;
      r->extent.height = (uint32_t)h;
   }
   return vk_outarray_status(&out);
}

static struct wsi_image *
panvk_android_swapchain_get_wsi_image(struct wsi_swapchain *swapchain,
                                     uint32_t image_index)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   return &chain->images[image_index].base;
}

static VkResult
panvk_android_swapchain_acquire_next_image(
   struct wsi_swapchain *swapchain,
   const VkAcquireNextImageInfoKHR *info,
   uint32_t *image_index)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;

   if (!chain->zero_copy) {
      uint32_t idx = chain->next_image;
      chain->next_image = (chain->next_image + 1) % chain->base.image_count;
      *image_index = idx;
      return VK_SUCCESS;
   }

   /* Zero-copy: take the next buffer the window hands out and map it back to
    * the image that wraps it. A buffer that was never imported means the
    * window reallocated its pool; hand it straight back and look again. */
   for (uint32_t attempt = 0; attempt < chain->base.image_count + 4; attempt++) {
      void *anb = NULL;
      int fence_fd = -1;
      if (panvk_android_anw.dequeue_buffer(chain->window, &anb, &fence_fd) != 0 ||
          !anb) {
         if (fence_fd >= 0)
            close(fence_fd);
         return VK_ERROR_OUT_OF_DATE_KHR;
      }

      int idx = panvk_android_swapchain_find_anb(chain, anb);
      if (idx < 0) {
         panvk_android_anw.cancel_buffer(chain->window, anb, -1);
         if (fence_fd >= 0)
            close(fence_fd);
         continue;
      }

      panvk_android_anw_wait_fence(fence_fd);
      chain->images[idx].held = true;
      chain->next_image = (idx + 1) % chain->base.image_count;
      *image_index = (uint32_t)idx;
      return VK_SUCCESS;
   }

   return VK_ERROR_OUT_OF_DATE_KHR;
}

static VkResult
panvk_android_swapchain_release_images(struct wsi_swapchain *swapchain,
                                       uint32_t count,
                                       const uint32_t *indices)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;

   for (uint32_t i = 0; i < count; i++) {
      if (indices[i] >= swapchain->image_count)
         return VK_ERROR_OUT_OF_DATE_KHR;

      /* Zero-copy: the app dropped the image without presenting it, so the
       * window buffer has to go back to the pool untouched. */
      struct panvk_android_swapchain_image *img = &chain->images[indices[i]];
      if (chain->zero_copy && img->held) {
         panvk_android_anw.cancel_buffer(chain->window, img->anb, -1);
         img->held = false;
      }

      swapchain->get_wsi_image(swapchain, indices[i])->acquired = false;
   }

   return VK_SUCCESS;
}

static VkResult
panvk_android_swapchain_wait_for_present2(struct wsi_swapchain *swapchain,
                                          uint64_t present_id,
                                          uint64_t timeout)
{
   struct panvk_android_swapchain *chain =
      (struct panvk_android_swapchain *)swapchain;

   return present_id <= chain->completed_present_id ? VK_SUCCESS : VK_TIMEOUT;
}

/* Bounding box of the damaged rectangles, clamped to the image extent.
 * Returns false when the app reported no usable damage (present it all). */
static bool
panvk_android_swapchain_damage_bounds(const VkPresentRegionKHR *damage,
                                      uint32_t width, uint32_t height,
                                      ARect *bounds)
{
   if (!damage || !damage->pRectangles || damage->rectangleCount == 0)
      return false;

   int32_t left = INT32_MAX, top = INT32_MAX, right = 0, bottom = 0;
   for (uint32_t i = 0; i < damage->rectangleCount; i++) {
      const VkRectLayerKHR *r = &damage->pRectangles[i];
      int64_t l = MAX2((int64_t)r->offset.x, 0);
      int64_t t = MAX2((int64_t)r->offset.y, 0);
      int64_t rr = MIN2((int64_t)r->offset.x + r->extent.width, (int64_t)width);
      int64_t bb = MIN2((int64_t)r->offset.y + r->extent.height, (int64_t)height);
      if (rr <= l || bb <= t)
         continue;
      left = MIN2(left, (int32_t)l);
      top = MIN2(top, (int32_t)t);
      right = MAX2(right, (int32_t)rr);
      bottom = MAX2(bottom, (int32_t)bb);
   }

   if (right <= left || bottom <= top)
      return false;

   *bounds = (ARect){.left = left, .top = top, .right = right, .bottom = bottom};
   return true;
}

static VkResult
panvk_android_swapchain_queue_present(
   struct wsi_swapchain *swapchain,
   uint32_t image_index,
   uint64_t present_id,
   const VkPresentRegionKHR *damage)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   if (image_index >= chain->base.image_count)
      return VK_ERROR_OUT_OF_DATE_KHR;

   /* Software WSI already waited on this image's fence in
    * wsi_common_queue_present() (the fence is signaled once the app's
    * rendering submit has completed), so the GPU no longer touches the AHB.
    * The dma-buf path does not wait and keeps the full device flush. */
   const struct wsi_device *wsi = swapchain->wsi;
   if (!wsi->sw || swapchain->sw_fence_wait_in_present_thread) {
      VK_FROM_HANDLE(vk_device, dev, swapchain->device);
      dev->dispatch_table.DeviceWaitIdle(swapchain->device);
   }

   if (chain->zero_copy) {
      struct panvk_android_swapchain_image *img = &chain->images[image_index];
      if (!img->held)
         return VK_ERROR_OUT_OF_DATE_KHR;

      /* The GPU already wrote straight into this buffer, so presenting is just
       * handing it back to the window. No fence: the software WSI waited for
       * the rendering submit above. */
      int32_t status =
         panvk_android_anw.queue_buffer(chain->window, img->anb, -1);
      img->held = false;
      if (status != 0) {
         panvk_android_anw.cancel_buffer(chain->window, img->anb, -1);
         return VK_ERROR_OUT_OF_DATE_KHR;
      }
   } else if (chain->window && chain->images[image_index].ahb) {
      const struct panvk_android_swapchain_image *img = &chain->images[image_index];
      ARect damage_bounds;
      ARect *p_damage_bounds =
         panvk_android_swapchain_damage_bounds(damage, img->width, img->height,
                                               &damage_bounds) ? &damage_bounds : NULL;
      ANativeWindow_Buffer win_buf;
      if (ANativeWindow_lock(chain->window, &win_buf, p_damage_bounds) == 0) {
         void *ahb_data = NULL;
         if (win_buf.bits && win_buf.width > 0 && win_buf.height > 0 &&
             AHardwareBuffer_lock(img->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                  -1, NULL, &ahb_data) == 0) {
            uint32_t copy_w = MIN2((uint32_t)win_buf.width, img->width);
            uint32_t copy_h = MIN2((uint32_t)win_buf.height, img->height);
            if (win_buf.stride == (int32_t)img->stride && copy_w == img->width) {
               /* Both sides are row-contiguous: copy in one go. */
               memcpy(win_buf.bits, ahb_data, (size_t)copy_h * img->stride * 4);
            } else {
               for (uint32_t y = 0; y < copy_h; y++) {
                  memcpy((char *)win_buf.bits + y * win_buf.stride * 4,
                         (char *)ahb_data + y * img->stride * 4,
                         copy_w * 4);
               }
            }
            AHardwareBuffer_unlock(img->ahb, NULL);
         }
         ANativeWindow_unlockAndPost(chain->window);
      }
   }

   chain->completed_present_id = MAX2(chain->completed_present_id, present_id);

   return VK_SUCCESS;
}

static VkResult
panvk_android_swapchain_destroy(
   struct wsi_swapchain *swapchain,
   const VkAllocationCallbacks *pAllocator)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   VK_FROM_HANDLE(vk_device, dev, swapchain->device);

   vk_android_set_owned_ahb(NULL);
   panvk_android_swapchain_free_images(chain, swapchain->device, pAllocator);

   wsi_swapchain_finish(&chain->base);
   vk_free2(&dev->alloc, pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_create_swapchain(
   VkIcdSurfaceBase *surface,
   VkDevice device,
   struct wsi_device *wsi_device,
   const VkSwapchainCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   struct wsi_swapchain **swapchain_out)
{
    VK_FROM_HANDLE(vk_device, dev, device);
    VkResult result;
    uint32_t num_images = pCreateInfo->minImageCount < 2 ? 2 : pCreateInfo->minImageCount;
    if (num_images > 4) num_images = 4;

    struct panvk_android_swapchain *chain = vk_zalloc2(
       &dev->alloc, pAllocator, sizeof(*chain), 8,
       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!chain)
       return VK_ERROR_OUT_OF_HOST_MEMORY;

    vk_object_base_init(dev, &chain->base.base, VK_OBJECT_TYPE_SWAPCHAIN_KHR);
    chain->base.wsi = wsi_device;
    chain->base.device = device;
    chain->base.alloc = *pAllocator;
    chain->base.blit.type = WSI_SWAPCHAIN_NO_BLIT;
    chain->base.destroy = panvk_android_swapchain_destroy;
    chain->base.get_wsi_image = panvk_android_swapchain_get_wsi_image;
   chain->base.acquire_next_image = panvk_android_swapchain_acquire_next_image;
   chain->base.queue_present = panvk_android_swapchain_queue_present;
   chain->base.wait_for_present2 = panvk_android_swapchain_wait_for_present2;
   chain->base.release_images = panvk_android_swapchain_release_images;
   chain->base.image_count = num_images;
   chain->next_image = 0;

   VkIcdSurfaceAndroid *android_surface = (VkIcdSurfaceAndroid *)surface;
   chain->window = android_surface->window;
   if (chain->window) {
      ANativeWindow_setBuffersGeometry(chain->window,
                                       pCreateInfo->imageExtent.width,
                                       pCreateInfo->imageExtent.height,
                                       WINDOW_FORMAT_RGBA_8888);
      mesa_logi("panvk: Android WSI swapchain %ux%u, %u images requested",
                pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                num_images);
   }

   if (chain->window && panvk_android_zero_copy_allowed()) {
      pthread_once(&panvk_android_anw_once, panvk_android_anw_resolve);

      uint32_t zc_images = num_images;
      if (panvk_android_swapchain_init_zero_copy(chain, device, pAllocator,
                                                 pCreateInfo, num_images,
                                                 &zc_images) == VK_SUCCESS) {
         chain->zero_copy = true;
         num_images = zc_images;
         mesa_logi("panvk: zero-copy present enabled: %u window images, "
                   "present is a single queueBuffer", num_images);
      } else {
         panvk_android_swapchain_free_images(chain, device, pAllocator);
         mesa_logi("panvk: zero-copy present unavailable, using the AHB copy "
                   "path");
      }
   }

   if (!chain->zero_copy) {
      result = panvk_android_swapchain_init_copy(chain, device, pAllocator,
                                                 pCreateInfo, num_images);
      if (result != VK_SUCCESS) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }
   }

   chain->base.image_count = num_images;

   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateAndroidSurfaceKHR(
   VkInstance _instance,
   const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   if (!pCreateInfo || !pCreateInfo->window)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkIcdSurfaceAndroid *surface = vk_alloc2(
      &instance->alloc, pAllocator, sizeof(*surface), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!surface)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_ANDROID;
   surface->window = pCreateInfo->window;
   ANativeWindow_acquire(surface->window);

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

static struct wsi_interface panvk_android_wsi_interface = {
   .get_support = panvk_wsi_android_get_support,
   .get_capabilities2 = panvk_wsi_android_get_capabilities2,
   .get_formats = panvk_wsi_android_get_formats,
   .get_formats2 = panvk_wsi_android_get_formats2,
   .get_present_modes = panvk_wsi_android_get_present_modes,
   .get_present_rectangles = panvk_wsi_android_get_present_rectangles,
   .create_swapchain = panvk_wsi_android_create_swapchain,
};
#endif /* VK_USE_PLATFORM_ANDROID_KHR */

VkResult
panvk_wsi_init(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   const bool uses_kbase = physical_device->kbase_node_path[0] != '\0';
   const char *dri3_option = getenv("PANVK_KBASE_DRI3");
   /* The proprietary Termux:X11 layer uses WSI_X11_TERMUX=1 as its
    * process-wide switch.  Accept the same switch here when no explicit
    * PanVK override is present, so both ICDs exercise the same raw-FD DRI3
    * transport instead of silently falling back to the slower SHM path. */
   const char *termux_wsi = getenv("WSI_X11_TERMUX");
   const bool termux_raw_dri3 =
      uses_kbase && !dri3_option && termux_wsi && strcmp(termux_wsi, "0") != 0;
   const bool kbase_raw_dri3 =
      termux_raw_dri3 ||
      (uses_kbase && dri3_option &&
       (!strcmp(dri3_option, "raw") || !strcmp(dri3_option, "termux")));
   const bool kbase_dmabuf =
#if defined(HAVE_PAN_KMOD_KBASE)
      uses_kbase &&
      ((!dri3_option && termux_raw_dri3) ||
       (dri3_option && strcmp(dri3_option, "0") != 0)) &&
      kbase_kmod_supports_dmabuf(physical_device->kmod.dev);
#else
      false;
#endif
   VkResult result;

   result = wsi_device_init(&physical_device->wsi_device,
                            panvk_physical_device_to_handle(physical_device),
                            panvk_wsi_proc_addr, &instance->vk.alloc, -1,
                            &instance->drirc.options,
                            &(struct wsi_device_options){
                               .sw_device = uses_kbase && !kbase_dmabuf,
                                  kbase_dmabuf && kbase_raw_dri3,
                            });
   if (result != VK_SUCCESS)
      return result;

   /* kbase syncs carry GPU seqno state in userspace and cannot be copied via
    * DRM syncobj fd payloads.  Keep even empty WSI submits on the real queue
    * so present fences are backed by the kbase submission timeline.
    */
   physical_device->wsi_device.disable_unordered_submits = uses_kbase;

   /* kbase is not a DRM fd.  The default CPU WSI path presents through
    * MIT-SHM.  Android dma-heaps can also provide shareable
    * allocations, but some Android X servers advertise DRI3 while rejecting
    * standard PixmapFromBuffer.  PANVK_KBASE_DRI3=raw (or termux) uses the
    * Termux:X11/Winlator private raw-FD modifier instead of DRM modifiers.
    */
   physical_device->wsi_device.supports_modifiers =
      !uses_kbase || (kbase_dmabuf && !kbase_raw_dri3);
   physical_device->wsi_device.can_present_on_device =
      panvk_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

#ifdef VK_USE_PLATFORM_ANDROID_KHR
   physical_device->wsi_device.wsi[VK_ICD_WSI_PLATFORM_ANDROID] = &panvk_android_wsi_interface;
#endif

   return VK_SUCCESS;
}

void
panvk_wsi_finish(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);

   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &instance->vk.alloc);
}
