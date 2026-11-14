/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "panvk_macros.h"
#include "panvk_buffer.h"

#include "genxml/decode.h"

#include "vulkan/util/vk_util.h"

#include "panvk_android.h"
#include "panvk_device.h"
#include "panvk_device_memory.h"

/* --- render activity counters (diagnostic) ---------------------------- */
unsigned long long panvk_diag_draws;
unsigned long long panvk_diag_renders;
unsigned long long panvk_diag_blits;
unsigned long long panvk_diag_copies;
unsigned long long panvk_diag_clears;
unsigned long long panvk_diag_present_writes;
unsigned long long panvk_diag_present_reads;
unsigned long long panvk_diag_present_clears;

static VkImage diag_bind_imgs[512];
static void *diag_bind_bos[512];
static unsigned diag_bind_n;
static void *diag_bind_hosts[512];
static uint64_t diag_bind_hash[512];
static bool diag_bind_valid[512];
static unsigned diag_sweep_tick;
static void panvk_diag_invalidate_cpu_range(const void *addr, size_t size);

static VkImage diag_blit_src;
static VkImage diag_rts[8];
static unsigned diag_rt_n;
static uint64_t diag_rt_hash[8];
static bool diag_rt_valid[8];

static void
diag_sweep_all(void)
{
   for (unsigned i = 0; i < diag_bind_n; i++) {
      struct pan_kmod_bo *bo = diag_bind_bos[i];

      if (bo == NULL)
         continue;

      if (diag_bind_hosts[i] == NULL) {
         void *h = pan_kmod_bo_mmap(bo, PROT_READ, MAP_SHARED, NULL);
         if (h == NULL || h == MAP_FAILED)
            continue;
         diag_bind_hosts[i] = h;
      }

      uint64_t bytes = pan_kmod_bo_size(bo);
      if (bytes > (uint64_t)1 * 1024 * 1024)
         bytes = (uint64_t)1 * 1024 * 1024;
      const uint64_t nwords = bytes / 4;
      panvk_diag_invalidate_cpu_range(diag_bind_hosts[i], bytes);
      const uint32_t *p = diag_bind_hosts[i];
      uint64_t nz = 0, hash = 1469598103934665603ull;

      for (uint64_t k = 0; k < nwords; k++) {
         uint32_t v = p[k];
         nz += (v != 0);
         hash = (hash ^ v) * 1099511628211ull;
      }

      const bool changed = diag_bind_valid[i] && (hash != diag_bind_hash[i]);
      PANVK_DIAG_PRINTF("[PANVK-DIAG] IMG[%u]: img=%p size=%llu MiB nz=%llu/%llu "
                        "hash=%016llx %s\n",
                        i, (void *)diag_bind_imgs[i],
                        (unsigned long long)(pan_kmod_bo_size(bo) >> 20),
                        (unsigned long long)nz, (unsigned long long)nwords,
                        (unsigned long long)hash, changed ? "CHANGED" : "same");
      diag_bind_hash[i] = hash;
      diag_bind_valid[i] = true;
   }
}

void
panvk_import_probe_set_render_target(VkImage img)
{
   if (img == VK_NULL_HANDLE)
      return;
   for (unsigned i = 0; i < diag_rt_n; i++) {
      if (diag_rts[i] == img)
         return;
   }
   if (diag_rt_n < 8)
      diag_rts[diag_rt_n++] = img;
}

static uint64_t diag_rt_draws[8];
static uint64_t diag_rt_other_draws;

unsigned long long
panvk_diag_rt_other_draws(void)
{
   return diag_rt_other_draws;
}

void
panvk_diag_note_rt_draw(VkImage img)
{
   if (img == VK_NULL_HANDLE) {
      diag_rt_other_draws++;
      return;
   }
   for (unsigned i = 0; i < diag_rt_n; i++) {
      if (diag_rts[i] == img) {
         diag_rt_draws[i]++;
         return;
      }
   }
   diag_rt_other_draws++;
}

static void
diag_scan_rt(unsigned idx)
{
   const VkImage img = diag_rts[idx];
   struct pan_kmod_bo *bo = NULL;

   for (unsigned i = 0; i < diag_bind_n; i++) {
      if (diag_bind_imgs[i] == img) {
         bo = diag_bind_bos[i];
         break;
      }
   }

   if (bo == NULL) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] RT[%u] probe: img=%p no bo\n", idx, (void *)img);
      return;
   }

   void *host = pan_kmod_bo_mmap(bo, PROT_READ, MAP_SHARED, NULL);
   if (host == NULL || host == MAP_FAILED) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] RT[%u] probe: img=%p no cpu map, size=%llu MiB\n",
                        idx, (void *)img, (unsigned long long)(pan_kmod_bo_size(bo) >> 20));
      return;
   }

   uint64_t bytes = pan_kmod_bo_size(bo);
   if (bytes > (uint64_t)16 * 1024 * 1024)
      bytes = (uint64_t)16 * 1024 * 1024;
   const uint64_t nwords = bytes / 4;
   panvk_diag_invalidate_cpu_range(host, bytes);
   const uint32_t *p = host;
   uint64_t nz = 0, hash = 1469598103934665603ull;

   for (uint64_t k = 0; k < nwords; k++) {
      uint32_t v = p[k];
      nz += (v != 0);
      hash = (hash ^ v) * 1099511628211ull;
   }

   const bool changed = diag_rt_valid[idx] && (hash != diag_rt_hash[idx]);
   PANVK_DIAG_PRINTF("[PANVK-DIAG] RT[%u] probe: img=%p size=%llu MiB nz=%llu/%llu "
                     "hash=%016llx draws=%llu %s\n",
                     idx, (void *)img, (unsigned long long)(pan_kmod_bo_size(bo) >> 20),
                     (unsigned long long)nz, (unsigned long long)nwords,
                     (unsigned long long)hash,
                     (unsigned long long)diag_rt_draws[idx],
                     changed ? "CHANGED" : "same");
   diag_rt_hash[idx] = hash;
   diag_rt_valid[idx] = true;
}

static void
diag_scan_image_bo(const char *tag, VkImage img)
{
   struct pan_kmod_bo *bo = NULL;

   if (img == VK_NULL_HANDLE)
      return;

   for (unsigned i = 0; i < diag_bind_n; i++) {
      if (diag_bind_imgs[i] == img) {
         bo = diag_bind_bos[i];
         break;
      }
   }

   if (bo == NULL) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] %s probe: img=%p no bo found\n", tag, (void *)img);
      return;
   }

   void *host = pan_kmod_bo_mmap(bo, PROT_READ, MAP_SHARED, NULL);
   if (host == NULL || host == MAP_FAILED) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] %s probe: img=%p no cpu map, size=%llu MiB\n",
                        tag, (void *)img, (unsigned long long)(pan_kmod_bo_size(bo) >> 20));
      return;
   }

   uint64_t bytes = pan_kmod_bo_size(bo);
   if (bytes > (uint64_t)16 * 1024 * 1024)
      bytes = (uint64_t)16 * 1024 * 1024;
   const uint64_t nwords = bytes / 4;
   panvk_diag_invalidate_cpu_range(host, bytes);
   const uint32_t *p = host;
   uint64_t nz = 0, hash = 1469598103934665603ull;

   for (uint64_t k = 0; k < nwords; k++) {
      uint32_t v = p[k];
      nz += (v != 0);
      hash = (hash ^ v) * 1099511628211ull;
   }

   PANVK_DIAG_PRINTF("[PANVK-DIAG] %s probe: img=%p size=%llu MiB nz=%llu/%llu "
                     "hash=%016llx w0=%08x w1=%08x w2=%08x w3=%08x\n",
                     tag, (void *)img,
                     (unsigned long long)(pan_kmod_bo_size(bo) >> 20),
                     (unsigned long long)nz, (unsigned long long)nwords,
                     (unsigned long long)hash,
                     p[0], p[1], p[2], p[3]);
}

void
panvk_import_probe_note_bind(VkImage img, void *bo)
{
   if (bo == NULL)
      return;
   for (unsigned i = 0; i < diag_bind_n; i++) {
      if (diag_bind_imgs[i] == img) {
         diag_bind_bos[i] = bo;
         return;
      }
   }
   if (diag_bind_n < 512) {
      diag_bind_imgs[diag_bind_n] = img;
      diag_bind_bos[diag_bind_n] = bo;
      diag_bind_n++;
   }
}

void
panvk_import_probe_set_blit_src(VkImage src, VkImage dst)
{
   if (panvk_import_probe_is_image(dst))
      diag_blit_src = src;
}

static VkImage import_probe_imgs[8];
static unsigned import_probe_img_count;
static VkBuffer import_probe_bufs[8];
static unsigned import_probe_buf_count;

static struct pan_kmod_bo *import_probe_bos[8];

static bool
import_probe_bo_match(void *bo)
{
   if (bo == NULL)
      return false;
   for (unsigned i = 0; i < 8; i++) {
      if (import_probe_bos[i] == bo)
         return true;
   }
   return false;
}

void
panvk_import_probe_track_image(VkImage img, void *bo)
{
   if (!import_probe_bo_match(bo))
      return;
   for (unsigned i = 0; i < import_probe_img_count; i++) {
      if (import_probe_imgs[i] == img)
         return;
   }
   if (import_probe_img_count < 8)
      import_probe_imgs[import_probe_img_count++] = img;
   PANVK_DIAG_PRINTF("[PANVK-DIAG] present IMAGE bound: img=%p bo=%p count=%u\n",
                     (void *)img, bo, import_probe_img_count);
}

void
panvk_import_probe_track_buffer(VkBuffer buf, void *bo)
{
   if (!import_probe_bo_match(bo))
      return;
   for (unsigned i = 0; i < import_probe_buf_count; i++) {
      if (import_probe_bufs[i] == buf)
         return;
   }
   if (import_probe_buf_count < 8)
      import_probe_bufs[import_probe_buf_count++] = buf;
   PANVK_DIAG_PRINTF("[PANVK-DIAG] present BUFFER bound: buf=%p bo=%p count=%u\n",
                     (void *)buf, bo, import_probe_buf_count);
}

bool
panvk_import_probe_is_image(VkImage img)
{
   for (unsigned i = 0; i < import_probe_img_count; i++) {
      if (import_probe_imgs[i] == img)
         return true;
   }
   return false;
}

bool
panvk_import_probe_is_buffer(VkBuffer buf)
{
   for (unsigned i = 0; i < import_probe_buf_count; i++) {
      if (import_probe_bufs[i] == buf)
         return true;
   }
   return false;
}

static VkBuffer diag_vbo_bufs[8];
static unsigned diag_vbo_count;

void
panvk_diag_note_draw(uint64_t vb_addr, uint32_t base, uint32_t count,
                     uint32_t instances)
{
   static unsigned n;

   if (n >= 8 || diag_vbo_count == 0)
      return;
   n++;

   VK_FROM_HANDLE(panvk_buffer, b, diag_vbo_bufs[0]);
   if (b == NULL || b->diag_bo == NULL) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] DRAW[%u]: no vbo tracking\n", n);
      return;
   }

   uint64_t delta = vb_addr >= b->vk.device_address ?
                    vb_addr - b->vk.device_address : ~0ull;

   void *host = pan_kmod_bo_mmap(b->diag_bo, PROT_READ, MAP_SHARED, NULL);
   if (host == NULL || host == MAP_FAILED || delta == ~0ull ||
       delta + 64 > b->vk.size) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] DRAW[%u]: vb=0x%llx delta=0x%llx base=%u "
                        "count=%u inst=%u unmappable\n", n,
                        (unsigned long long)vb_addr, (unsigned long long)delta,
                        base, count, instances);
      return;
   }

   const uint32_t *p = (const uint32_t *)((char *)host + b->diag_mem_off + delta);
   panvk_diag_invalidate_cpu_range(p, 64);

   float f_stride4 = 0, f_stride8 = 0, f_stride16 = 0;
   if (delta + (uint64_t)base * 16 + 4 <= b->vk.size) {
      const uint32_t *q4 = (const uint32_t *)((const char *)p + (uint64_t)base * 4);
      const uint32_t *q8 = (const uint32_t *)((const char *)p + (uint64_t)base * 8);
      const uint32_t *q16 = (const uint32_t *)((const char *)p + (uint64_t)base * 16);
      panvk_diag_invalidate_cpu_range(q4, 4);
      f_stride4 = *(const float *)&q4[0];
      f_stride8 = *(const float *)&q8[0];
      f_stride16 = *(const float *)&q16[0];
   }

   PANVK_DIAG_PRINTF("[PANVK-DIAG] DRAW[%u]: vb=0x%llx delta=0x%llx base=%u "
                     "count=%u inst=%u | w0=%08x(%g) w1=%08x(%g) w2=%08x(%g) "
                     "w3=%08x(%g) | basef4=%g f8=%g f16=%g\n",
                     n, (unsigned long long)vb_addr, (unsigned long long)delta,
                     base, count, instances,
                     p[0], (double)*(const float *)&p[0],
                     p[1], (double)*(const float *)&p[1],
                     p[2], (double)*(const float *)&p[2],
                     p[3], (double)*(const float *)&p[3],
                     (double)f_stride4, (double)f_stride8, (double)f_stride16);
}

void
panvk_diag_note_vbo(VkBuffer buf)
{
   for (unsigned i = 0; i < diag_vbo_count; i++) {
      if (diag_vbo_bufs[i] == buf)
         return;
   }
   if (diag_vbo_count < 8)
      diag_vbo_bufs[diag_vbo_count++] = buf;
}

static void
panvk_diag_invalidate_cpu_range(const void *addr, size_t size)
{
#if defined(__aarch64__)
   uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63;
   uintptr_t end = ((uintptr_t)addr + size + 63) & ~(uintptr_t)63;
   for (uintptr_t p = start; p < end; p += 64)
      __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
   __asm__ volatile("dsb sy" : : : "memory");
#else
   (void)addr;
   (void)size;
#endif
}

/* --- imported present buffer probe (diagnostic) ----------------------- */
static uint32_t import_probe_count;
static uint64_t import_probe_submits;

void
panvk_import_probe_tick(void)
{
   return; /* rc40-clean: sweeps disabled */
   if (import_probe_count == 0)
      return;

   if ((++import_probe_submits % 32) != 0)
      return;

   for (uint32_t i = 0; i < 8; i++) {
      struct pan_kmod_bo *bo = import_probe_bos[i];

      if (bo == NULL)
         continue;

      void *host = pan_kmod_bo_mmap(bo, PROT_READ, MAP_SHARED, NULL);
      if (host == NULL || host == MAP_FAILED) {
         PANVK_DIAG_PRINTF("[PANVK-DIAG] import probe %u: no cpu map, size=%llu MiB\n",
                           i, (unsigned long long)(pan_kmod_bo_size(bo) >> 20));
         continue;
      }

      uint64_t bytes = pan_kmod_bo_size(bo);
      if (bytes > (uint64_t)16 * 1024 * 1024)
         bytes = (uint64_t)16 * 1024 * 1024;
      const uint64_t nwords = bytes / 4;
      panvk_diag_invalidate_cpu_range(host, bytes);
      const uint32_t *p = host;
      uint64_t nz = 0, hash = 1469598103934665603ull;

      for (uint64_t k = 0; k < nwords; k++) {
         uint32_t v = p[k];
         nz += (v != 0);
         hash = (hash ^ v) * 1099511628211ull;
      }

      PANVK_DIAG_PRINTF("[PANVK-DIAG] import probe %u: size=%llu MiB nz=%llu/%llu "
                        "hash=%016llx w0=%08x w1=%08x w2=%08x w3=%08x submits=%llu "
                        "draws=%llu renders=%llu blits=%llu copies=%llu clears=%llu "
                        "pwrites=%llu preads=%llu\n",
                        i, (unsigned long long)(pan_kmod_bo_size(bo) >> 20),
                        (unsigned long long)nz, (unsigned long long)nwords,
                        (unsigned long long)hash,
                        p[0], p[1], p[2], p[3],
                        (unsigned long long)import_probe_submits,
                        panvk_diag_draws, panvk_diag_renders,
                        panvk_diag_blits, panvk_diag_copies, panvk_diag_clears,
                        panvk_diag_present_writes, panvk_diag_present_reads);
   }

   for (unsigned i = 0; i < diag_vbo_count; i++) {
      VK_FROM_HANDLE(panvk_buffer, b, diag_vbo_bufs[i]);

      if (b == NULL || b->diag_bo == NULL)
         continue;

      void *host = pan_kmod_bo_mmap(b->diag_bo, PROT_READ, MAP_SHARED, NULL);
      if (host == NULL || host == MAP_FAILED) {
         PANVK_DIAG_PRINTF("[PANVK-DIAG] VBO[%u]: no cpu map size=%llu\n", i,
                           (unsigned long long)b->vk.size);
         continue;
      }

      uint64_t bytes = MIN2(b->vk.size, (uint64_t)65536);
      const uint32_t *p = (const uint32_t *)((char *)host + b->diag_mem_off);
      panvk_diag_invalidate_cpu_range(p, bytes);
      uint64_t nz = 0, nwords = bytes / 4;

      for (uint64_t k = 0; k < nwords; k++)
         nz += (p[k] != 0);

      PANVK_DIAG_PRINTF("[PANVK-DIAG] VBO[%u]: size=0x%llx off=0x%llx nz=%llu/%llu "
                        "w0=%08x(%g) w1=%08x(%g) w2=%08x(%g) w3=%08x(%g)\n",
                        i, (unsigned long long)b->vk.size,
                        (unsigned long long)b->diag_mem_off,
                        (unsigned long long)nz, (unsigned long long)nwords,
                        p[0], (double)*(const float *)&p[0],
                        p[1], (double)*(const float *)&p[1],
                        p[2], (double)*(const float *)&p[2],
                        p[3], (double)*(const float *)&p[3]);

      pan_kmod_bo_munmap(b->diag_bo, host, 0);
   }

   diag_scan_image_bo("blit SRC", diag_blit_src);
   for (unsigned i = 0; i < diag_rt_n; i++)
      diag_scan_rt(i);

   panvk_sparse_mirror_probe();

   if ((diag_sweep_tick++ % 8) == 0)
      diag_sweep_all();
}
#include "panvk_entrypoints.h"

#include "pan_props.h"

#include "util/u_atomic.h"
#include "vk_debug_utils.h"
#include "vk_log.h"

static void
panvk_memory_emit_report(struct panvk_device *device,
                         const struct panvk_device_memory *mem,
                         const VkMemoryAllocateInfo *alloc_info,
                         VkResult result)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);

   if (likely(!device->vk.memory_reports))
      return;

   if (result != VK_SUCCESS) {
      const uint32_t heap_index =
         pdev->memory.types[alloc_info->memoryTypeIndex].heapIndex;
      vk_emit_device_memory_report(
         &device->vk, VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT,
         /* mem_obj_id */ 0, alloc_info->allocationSize,
         VK_OBJECT_TYPE_DEVICE_MEMORY,
         /* obj_handle */ 0, heap_index);
      return;
   }

   VkDeviceMemoryReportEventTypeEXT type;
   if (alloc_info) {
      type = mem->vk.import_handle_type
                ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT
                : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT;
   } else {
      type = mem->vk.import_handle_type
                ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT
                : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT;
   }

   const uint32_t heap_index =
      pdev->memory.types[mem->vk.memory_type_index].heapIndex;
   vk_emit_device_memory_report(&device->vk, type, mem->bo->handle,
                                mem->bo->size, VK_OBJECT_TYPE_DEVICE_MEMORY,
                                (uintptr_t)(mem), heap_index);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_AllocateMemory(VkDevice _device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMem)
{
   if (panvk_android_is_ahb_memory(pAllocateInfo)) {
      return panvk_android_allocate_ahb_memory(_device, pAllocateInfo,
                                               pAllocator, pMem);
   }

   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);
   struct panvk_device_memory *mem;
   bool can_be_exported = false;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize >= (8ull << 20)) {
      const VkImportMemoryFdInfoKHR *fd_dbg =
         vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
      const VkMemoryDedicatedAllocateInfo *ded_dbg =
         vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
      PANVK_DIAG_PRINTF("[PANVK-DIAG] AllocateMemory req size=%llu MiB type=%u "
              "handle_type=%d export=%d dedicated=%d\n",
              (unsigned long long)(pAllocateInfo->allocationSize >> 20),
              pAllocateInfo->memoryTypeIndex,
              fd_dbg != NULL ? (int)fd_dbg->handleType : -1,
              (int)(vk_find_struct_const(pAllocateInfo->pNext,
                                         EXPORT_MEMORY_ALLOCATE_INFO) != NULL),
              (int)(ded_dbg != NULL));
   }

   const VkMemoryType *type =
      &physical_device->memory.types[pAllocateInfo->memoryTypeIndex];

   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);

   if (export_info) {
      if (export_info->handleTypes &
          ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
         return panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      else if (export_info->handleTypes)
         can_be_exported = true;
   }

   mem = vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (mem == NULL)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);

   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (fd_info) {
      assert(
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      mem->bo = pan_kmod_bo_import(device->kmod.dev, fd_info->fd);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
   } else {
      uint32_t bo_flags = 0;

      /* We don't do cached on exported buffers to keep the pre-WB_MMAP
       * behavior.
       */
      if (!can_be_exported &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         bo_flags |= PAN_KMOD_BO_FLAG_WB_MMAP;

      /* A placed map aliases the BO at a caller-chosen address; on kbase
       * only shareable (dma-buf backed) BOs can be mapped twice. */
      bool shareable =
         can_be_exported ||
         (device->vk.enabled_features.memoryMapPlaced &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));

      bo_flags = panvk_device_adjust_bo_flags(device, bo_flags);
      mem->bo = pan_kmod_bo_alloc(device->kmod.dev,
                                  shareable ? NULL : device->kmod.vm,
                                  pAllocateInfo->allocationSize, bo_flags);
      if (!mem->bo) {
         PANVK_DIAG_PRINTF("[PANVK-DIAG] AllocateMemory FAIL bo size=%llu type=%u heap_used=%llu MiB\n",
                 (unsigned long long)pAllocateInfo->allocationSize,
                 pAllocateInfo->memoryTypeIndex,
                 (unsigned long long)(p_atomic_read(&physical_device->memory.heap_used) >> 20));
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_destroy_mem;
      }
   }

   /* Always GPU-map at creation time. */
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(mem->bo),
      },
      .map = {
         .bo = mem->bo,
         .bo_offset = 0,
      },
   };

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      uint64_t alignment =
         pan_choose_gpu_va_alignment(device->kmod.vm, op.va.size);
      unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
      /* For sizes larger than or equal to 64k, align the VA on 64k to meet the
       * requirement for interleaved_64k images (added in v10). */
      if (arch >= 10 && op.va.size >= 64 * 1024)
         alignment = MAX2(alignment, 64 * 1024);

      if (unlikely(mem->vk.alloc_flags &
                   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)) {
         const VkMemoryOpaqueCaptureAddressAllocateInfo *capture_alloc_info =
            vk_find_struct_const(pAllocateInfo->pNext,
                                 MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
         if (capture_alloc_info == NULL ||
             capture_alloc_info->opaqueCaptureAddress == 0) {
            op.va.start = panvk_as_alloc(device, &device->as.fixed_heap,
                                         op.va.size, alignment);
         } else {
            op.va.start = panvk_as_alloc_fixed_address(
               device, &device->as.fixed_heap,
               capture_alloc_info->opaqueCaptureAddress, op.va.size);
         }
      } else {
         op.va.start =
            panvk_as_alloc(device, &device->as.heap, op.va.size, alignment);
      }

      if (!op.va.start) {
         PANVK_DIAG_PRINTF("[PANVK-DIAG] AllocateMemory FAIL va size=%llu type=%u heap_used=%llu MiB\n",
                 (unsigned long long)pAllocateInfo->allocationSize,
                 pAllocateInfo->memoryTypeIndex,
                 (unsigned long long)(p_atomic_read(&physical_device->memory.heap_used) >> 20));
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_put_bo;
      }
   }

   int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] AllocateMemory FAIL bind size=%llu type=%u ret=%d\n",
              (unsigned long long)pAllocateInfo->allocationSize,
              pAllocateInfo->memoryTypeIndex, ret);
      result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_return_va;
   }

   mem->addr.dev = op.va.start;

   if (fd_info &&
       fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
      import_probe_bos[import_probe_count % 8] = mem->bo;
      import_probe_count++;
   }

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

   if (fd_info) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   if (device->debug.decode_ctx) {
      if (PANVK_DEBUG(DUMP) || PANVK_DEBUG(TRACE)) {
         void *cpu =
            pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
         if (cpu != MAP_FAILED)
            mem->debug.host_mapping = cpu;
         else
            vk_logw(VK_LOG_OBJS(_device),
                    "failed to map VkMemory for dump or trace.\n");
      }

      pandecode_inject_mmap(device->debug.decode_ctx, mem->addr.dev,
                            mem->debug.host_mapping, pan_kmod_bo_size(mem->bo),
                            NULL);
   }

   panvk_memory_emit_report(device, mem, pAllocateInfo, VK_SUCCESS);

   p_atomic_add(&physical_device->memory.heap_used,
                (uint64_t)pan_kmod_bo_size(mem->bo));

   if (pAllocateInfo->allocationSize >= (8ull << 20))
      PANVK_DIAG_PRINTF("[PANVK-DIAG] AllocateMemory ok size=%llu MiB type=%u "
              "addr=0x%llx heap_used=%llu MiB\n",
              (unsigned long long)(pAllocateInfo->allocationSize >> 20),
              pAllocateInfo->memoryTypeIndex,
              (unsigned long long)mem->addr.dev,
              (unsigned long long)(p_atomic_read(&physical_device->memory.heap_used) >> 20));

   *pMem = panvk_device_memory_to_handle(mem);

   return VK_SUCCESS;

err_return_va:
   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      panvk_as_free(device, &device->as.heap, op.va.start, op.va.size);
   }

err_put_bo:
   pan_kmod_bo_put(mem->bo);

err_destroy_mem:
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
   panvk_memory_emit_report(device, /* mem */ NULL, pAllocateInfo, result);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_FreeMemory(VkDevice _device, VkDeviceMemory _mem,
                 const VkAllocationCallbacks *pAllocator)
{
   panvk_sparse_mirror_forget(VK_NULL_HANDLE, _mem);

   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);

   p_atomic_add(&physical_device->memory.heap_used,
                -((int64_t)pan_kmod_bo_size(mem->bo)));

   if (device->debug.decode_ctx) {
      pandecode_inject_free(device->debug.decode_ctx, mem->addr.dev,
                            pan_kmod_bo_size(mem->bo));

      if (mem->debug.host_mapping)
         pan_kmod_bo_munmap(mem->bo, mem->debug.host_mapping,
                            pan_kmod_bo_size(mem->bo));
   }

   if (mem->addr.host) {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = mem->addr.dev,
         .size = pan_kmod_bo_size(mem->bo),
      },
   };

   ASSERTED int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      const bool fixed = (mem->vk.alloc_flags &
                          VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT);
      struct util_vma_heap *heap =
         fixed ? &device->as.fixed_heap : &device->as.heap;
      panvk_as_free(device, heap, op.va.start, op.va.size);
   }

   panvk_memory_emit_report(device, mem, /* alloc_info */ NULL, VK_SUCCESS);

   pan_kmod_bo_put(mem->bo);
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                    void **ppData)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "requested size 0x%" PRIx64
                          " does not fit in %u bits",
                          size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->addr.host)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object already mapped.");

   void *host_addr = NULL;
   int map_flags = MAP_SHARED;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info = vk_find_struct_const(
         pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      assert(placed_info != NULL);
      assert(offset == 0 && size == mem->bo->size);
      host_addr = placed_info->pPlacedAddress;
      map_flags |= MAP_FIXED;
   }

   void *addr =
      pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, map_flags, host_addr);
   if (addr == MAP_FAILED)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object couldn't be mapped.");

   assert(!host_addr || addr == host_addr);
   mem->addr.host = addr;
   {
      static uint32_t map_count;
      if (map_count++ < 64)
         PANVK_DIAG_PRINTF("[PANVK-DIAG] MapMemory size=0x%llx off=0x%llx "
                 "mem_va=0x%llx host=%p\n",
                 (unsigned long long)size, (unsigned long long)offset,
                 (unsigned long long)mem->addr.dev, addr);
   }
   *ppData = (char *)addr + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_UnmapMemory2KHR(VkDevice _device,
                      const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      void *reserved =
         os_mmap(mem->addr.host, pan_kmod_bo_size(mem->bo), PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (reserved == MAP_FAILED)
         return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                             "Failed to reserve VA on unmap.");
   } else {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   mem->addr.host = NULL;

   return VK_SUCCESS;
}

static VkResult
sync_memory_ranges(struct panvk_device *device, uint32_t memoryRangeCount,
                   const VkMappedMemoryRange *pMemoryRanges,
                   enum pan_kmod_bo_sync_type type)
{
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *r = &pMemoryRanges[i];
      if (r->size == 0)
         continue;

      VK_FROM_HANDLE(panvk_device_memory, memory, r->memory);

      pan_kmod_queue_bo_map_sync(
         memory->bo, r->offset, (char *)memory->addr.host + r->offset,
         vk_device_memory_range(&memory->vk, r->offset, r->size), type);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   return sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                             PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VkResult result =
      sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                         PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE);

   if (result == VK_SUCCESS)
      pan_kmod_flush_bo_map_syncs(device->kmod.dev);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = pan_kmod_bo_export(memory->bo);
   if (prime_fd < 0)
      return panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdPropertiesKHR(VkDevice _device,
                               VkExternalMemoryHandleTypeFlagBits handleType,
                               int fd,
                               VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);

   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

#if defined(HAVE_PAN_KMOD_KBASE)
   /* On kbase, a full transient import per query is not free: repeated
    * import/free cycles of one dma-buf alias to a single GPU VA, and the
    * frees can tear down GPU PTEs that re-import does not restore,
    * breaking the later real import (GPU TRANSLATION_FAULT). Answer from
    * the same assumptions the fd-import path uses instead.
    */
   if (phys_dev->kbase_node_path[0]) {
      /* fd < 0 is the only real error: MTK (and other vendor) AFBC gralloc
       * dma-bufs do not implement llseek, so probing with lseek() falsely
       * rejects valid ANativeWindow buffers and kills the whole WSI
       * swapchain (black screen with working audio). The memoryTypeBits
       * below are static assumptions anyway; the real import validates
       * the fd on its own.
       */
      if (fd < 0)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;

      /* Matches kbase_kmod_bo_import_fd(..., kmod_flags=0, external): */
      const bool can_do_host_visible = true;
      const bool can_do_host_coherent = true;
      const bool can_do_host_cached = false;
      pMemoryFdProperties->memoryTypeBits = 0;
      for (uint32_t i = 0; i < phys_dev->memory.type_count; i++) {
         if (!can_do_host_visible && (phys_dev->memory.types[i].propertyFlags &
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
         if (!can_do_host_coherent && (phys_dev->memory.types[i].propertyFlags &
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            continue;
         if (!can_do_host_cached && (phys_dev->memory.types[i].propertyFlags &
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            continue;
         pMemoryFdProperties->memoryTypeBits |= BITFIELD_BIT(i);
      }
      return VK_SUCCESS;
   }
#endif

   struct pan_kmod_bo *bo = pan_kmod_bo_import(device->kmod.dev, fd);
   if (!bo)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   pMemoryFdProperties->memoryTypeBits = 0;

   /* Keep things simple by only allowing host-visible if the BO doesn't require
    * kernel-side synchronization going through the dma-buf exporter, which is
    * reflected through the PAN_KMOD_BO_FLAG_FORCE_FULL_KERNEL_SYNC flag.
    */
   const bool can_do_host_visible = !(bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP);
   const bool can_do_host_coherent = !(bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP) ||
                                     (bo->flags & PAN_KMOD_BO_FLAG_IO_COHERENT);
   const bool can_do_host_cached = (bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP);

   pMemoryFdProperties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < phys_dev->memory.type_count; i++) {
      if (!can_do_host_visible && (phys_dev->memory.types[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         continue;
      if (!can_do_host_coherent && (phys_dev->memory.types[i].propertyFlags &
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         continue;
      if (!can_do_host_cached && (phys_dev->memory.types[i].propertyFlags &
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         continue;

      pMemoryFdProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }

   pan_kmod_bo_put(bo);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR uint64_t VKAPI_CALL
panvk_GetDeviceMemoryOpaqueCaptureAddress(
   VkDevice _device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   VK_FROM_HANDLE(panvk_device_memory, memory, pInfo->memory);

   return memory->addr.dev;
}

VKAPI_ATTR void VKAPI_CALL
panvk_SetDeviceMemoryPriorityEXT(VkDevice device, VkDeviceMemory memory,
                                 float priority)
{
}

