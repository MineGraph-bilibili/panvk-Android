/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_H
#define PANVK_DEVICE_H

#include <stdint.h>

#include "vk_debug_utils.h"
#include "vk_device.h"
#include "vk_meta.h"

#include "panvk_blend.h"
#include "panvk_instance.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_physical_device.h"
#include "panvk_utrace_perfetto.h"

#include "kmod/pan_kmod.h"
#include "kmod/panthor_kmod.h"
#ifdef HAVE_PAN_KMOD_KBASE
#include "kmod/kbase_kmod.h"
#endif
#include "util/perf/u_trace.h"

#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/u_queue.h"
#include "util/u_call_once.h"
#include "util/u_printf.h"
#include "util/vma.h"

/* On JM hardware, we need to allocate a buffer depending on vertex count.
 *
 * As a result, for indirect and indexed draw we allocate a large buffer with
 * alloc on fault set.
 *
 * The size of that buffer is calculated assuming a max of 2 millions vertices
 * and 18 attributes per vertex (16 user attributes, 2 specials)
 */

#define PANVK_JM_MAX_VERTICES_INDIRECT                (2000000)
#define PANVK_JM_MAX_PER_VTX_ATTRIBUTES_INDIRECT_SIZE (18 * 4)

struct panvk_precomp_cache;
struct panvk_device_draw_context;

enum panvk_queue_family {
   PANVK_QUEUE_FAMILY_GPU,
   PANVK_QUEUE_FAMILY_BIND,
   PANVK_QUEUE_FAMILY_COUNT,
};

struct panvk_upload_cache_entry;

struct panvk_device {
   struct vk_device vk;

   struct {
      simple_mtx_t lock;
      struct util_vma_heap heap;
      struct util_vma_heap fixed_heap;
      struct util_vma_heap *priv_heap;
      bool split_heap;
      bool extended_range;
   } as;

   struct {
      struct pan_kmod_vm *vm;
      struct pan_kmod_dev *dev;
      struct pan_kmod_allocator allocator;
   } kmod;

   struct panvk_priv_bo *tiler_heap;
   /* v9: second heap for the odd passes of PANVK_OVERLAP (v67b), so each
    * heap grows from its own base (kbase commits up to the faulting page). */
   struct panvk_priv_bo *tiler_heap2;
   struct panvk_priv_bo *indirect_varying_buffer;
   struct panvk_priv_bo *sample_positions;
   /* BC4/BC5 -> EAC transcode table, created on first use. */
   struct panvk_priv_bo *bc_eac_lut;

   /* Background prewarm of the BC transcode pipelines, submitted when the
    * first emulated BC image is created. bc_pipeline_mtx serialises pipeline
    * creation between the worker and the first transcoding command buffer,
    * and guards bc_prewarm_started. */
   struct util_queue bc_prewarm;
   simple_mtx_t bc_pipeline_mtx;
   bool bc_prewarm_started;

   /* Content-addressed decoded-BC cache: source-content hash -> an image whose
    * decoded plane still holds those texels, so a second image receiving the
    * same bytes copies decoded data instead of re-running the transcoder.
    * Guarded by upload_cache_lock; entries are dropped when the image dies. */
   struct panvk_upload_cache_entry *upload_cache;
   unsigned upload_cache_count;
   simple_mtx_t upload_cache_lock;

   struct {
      struct panvk_priv_bo *handlers_bo;
      uint32_t handler_stride;
   } tiler_oom;

   struct vk_meta_device meta;

   /* Bound in place of a missing FS so the depth bounds test still runs. */
   struct panvk_shader *depth_bounds_fs;

   struct {
      struct panvk_pool rw;
      struct panvk_pool rw_nc;
      struct panvk_pool exec;
   } mempools;

   struct {
      util_once_flag blackhole_once;
      struct pan_kmod_bo *blackhole;
   } sparse_mem;

   /* For each subqueue, maximum size of the register dump region needed by
    * exception handlers or functions */
   uint32_t *dump_region_size;

   struct vk_device_dispatch_table cmd_dispatch;

   struct panvk_precomp_cache *precomp_cache;

   struct {
      struct u_trace_context utctx;
#ifdef HAVE_PERFETTO
      struct panvk_utrace_perfetto utp;
#endif
      /* Timestamp + indirect data storage */
      struct util_vma_heap copy_buf_heap;
      struct panvk_priv_bo *copy_buf_heap_bo;
      simple_mtx_t copy_buf_heap_lock;
   } utrace;

   struct panvk_device_draw_context* draw_ctx;
   /* Allocated (and fully committed) at the first gpu_prerast /
    * tessellation draw recorded on the device, under gpu_prerast_lock,
    * unless PANVK_GPU_PRERAST_ARENA=eager (csf-v11/205). Read them through
    * panvk_per_arch(device_gpu_prerast_arena)(). gpu_prerast_wanted /
    * gpu_prerast_tess_wanted say whether the device may use them. */
   struct panvk_priv_mem gpu_prerast_arena;
   struct panvk_priv_mem gpu_prerast_tess_arena;
   /* Second tessellation arena, CSF only, at the first draw that picks it
    * (csf-v11 tess-arenas, see device_gpu_prerast_tess_arena2()). */
   struct panvk_priv_mem gpu_prerast_tess_arena2;
   bool gpu_prerast_wanted;
   bool gpu_prerast_tess_wanted;
   simple_mtx_t gpu_prerast_lock;
   /* struct panvk_gpu_prerast_syncs */
   struct panvk_priv_bo *gpu_prerast_syncs;

   /* CSF tessellation fast paths, read once at device creation so that a
    * process can A/B them per VkDevice. */
   struct {
      /* PANVK_TESS_FUSE (default 1): panlib_tess_count_draw(). */
      bool fuse;
      /* PANVK_TESS_FUSE_GEN_MAX (default 64): patches of a direct draw the
       * fused workgroup also tessellates. */
      uint32_t fuse_gen_max;
      /* PANVK_TESS_SETUP_STEP (default 1): panlib_tess_setup_step(). */
      bool setup_in_step;
      /* PANVK_TESS_ARENAS (default 2): tessellation arenas a command
       * buffer alternates its tess draws over. */
      uint8_t arenas;
      /* PANVK_TESS_SINGLE_CSG (default 1): a direct tess draw whose patch
       * count fits one iteration runs no TESS_FREE hand-off, see
       * gpu_prerast_tess(). */
      bool single_csg;
   } tess_opt;

   /* Bumped on every descriptor update. The CmdDrawIndexed batcher reads it
    * when starting a batch and at every draw, so an update between two draws
    * cannot silently reuse the old descriptor contents. */
   uint64_t desc_update_gen;

   struct {
      simple_mtx_t lock;
      struct util_dynarray bos;
      /* Sparse TLS alias state, per device. A permanent failure (the ioctl is
       * unsupported) must disable aliasing for this device only; a transient
       * one is retried after a backoff instead of falling back for the whole
       * process lifetime. */
      bool sparse_disabled;
      uint64_t sparse_retry_ns;
   } tls;

   struct {
      struct pandecode_context *decode_ctx;
   } debug;

   struct {
      struct u_printf_ctx ctx;
      struct panvk_priv_bo *bo;
   } printf;

   union {
      struct {
         struct {
            uint8_t count;
            uint8_t iter_count;
            uint16_t all_mask;
            uint16_t all_iters_mask;
         } sb;
      } csf;
   };

   int drm_fd;
};

VK_DEFINE_HANDLE_CASTS(panvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

#ifdef PAN_ARCH
void panvk_per_arch(bc_prewarm)(struct panvk_device *dev);
void panvk_per_arch(bc_prewarm_finish)(struct panvk_device *dev);
#endif

static inline struct panvk_device *
to_panvk_device(struct vk_device *dev)
{
   return container_of(dev, struct panvk_device, vk);
}

/* CSF interface information, sourced from the panthor uAPI or from the
 * kbase GLB interface depending on which backend the device came from.
 * Always use this instead of calling panthor_kmod_get_csif_props()
 * directly, which reads garbage on a kbase device. */
static inline const struct drm_panthor_csif_info *
panvk_get_csif_props(const struct panvk_device *dev)
{
#if defined(HAVE_PAN_KMOD_KBASE) && !defined(HAVE_PAN_KMOD_PANTHOR)
   return kbase_kmod_get_csif_props(dev->kmod.dev);
#elif defined(HAVE_PAN_KMOD_KBASE)
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   if (phys_dev->kbase_node_path[0])
      return kbase_kmod_get_csif_props(dev->kmod.dev);

   return panthor_kmod_get_csif_props(dev->kmod.dev);
#else
   return panthor_kmod_get_csif_props(dev->kmod.dev);
#endif
}

/* Latest cache-flush ID, sourced from the panthor uAPI or the kbase CSF
 * USER register page depending on which backend the device came from. */
static inline uint32_t
panvk_get_flush_id(const struct panvk_device *dev)
{
#if defined(HAVE_PAN_KMOD_KBASE) && !defined(HAVE_PAN_KMOD_PANTHOR)
   return kbase_kmod_get_flush_id(dev->kmod.dev);
#elif defined(HAVE_PAN_KMOD_KBASE)
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   if (phys_dev->kbase_node_path[0])
      return kbase_kmod_get_flush_id(dev->kmod.dev);

   return panthor_kmod_get_flush_id(dev->kmod.dev);
#else
   return panthor_kmod_get_flush_id(dev->kmod.dev);
#endif
}

static inline void
panvk_address_binding_report(struct panvk_device *dev,
                             struct vk_object_base *object, uint64_t base,
                             uint64_t size, VkDeviceAddressBindingTypeEXT type)
{
   vk_address_binding_report(dev->vk.physical->instance,
                             object ? object : &dev->vk.base, base, size, type);
}

static inline uint32_t
panvk_device_adjust_bo_flags(const struct panvk_device *device,
                             uint32_t bo_flags)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);

   /* Dumping of GPU buffers messes up with CPU caches by loading buffers
    * into cachelines which are sometimes assumed to be invalidated by the
    * rest of the code. We could explicitly invalidate after a dump, but
    * this is simpler to just disable cached-mappings when PANVK_DEBUG="dump".
    * Tracing has the same fundamental issue, but we'll take care of that
    * at the desc/CS pools level.
    */
   const bool allow_wb_mmap =
      (pdev->kmod.dev->props.supported_bo_flags & PAN_KMOD_BO_FLAG_WB_MMAP) &&
      !PANVK_DEBUG(DUMP) && !PANVK_DEBUG(NO_WB_MMAP);

   if (!allow_wb_mmap)
      bo_flags &= ~PAN_KMOD_BO_FLAG_WB_MMAP;

   if (PANVK_DEBUG(DUMP) || PANVK_DEBUG(TRACE))
      bo_flags &= ~PAN_KMOD_BO_FLAG_NO_MMAP;

   if (!(device->kmod.dev->props.supported_bo_flags &
         PAN_KMOD_BO_FLAG_GPU_UNCACHED))
      bo_flags &= ~PAN_KMOD_BO_FLAG_GPU_UNCACHED;

   return bo_flags;
}

static inline uint64_t
panvk_get_gpu_page_size(const struct panvk_device *device)
{
   return (uint64_t)1 << (ffsll(device->kmod.vm->pgsize_bitmap) - 1);
}

static inline uint64_t
panvk_as_alloc(struct panvk_device *device, struct util_vma_heap *heap,
               uint64_t size, uint64_t alignment)
{
   simple_mtx_lock(&device->as.lock);
   uint64_t address = util_vma_heap_alloc(heap, size, alignment);
   simple_mtx_unlock(&device->as.lock);
   return address;
}

static inline uint64_t
panvk_as_alloc_fixed_address(struct panvk_device *device,
                             struct util_vma_heap *heap, uint64_t address,
                             uint64_t size)
{
   simple_mtx_lock(&device->as.lock);
   bool alloc_result = util_vma_heap_alloc_addr(heap, address, size);
   simple_mtx_unlock(&device->as.lock);
   return alloc_result ? address : 0;
}

static inline void
panvk_as_free(struct panvk_device *device, struct util_vma_heap *heap,
              uint64_t address, uint64_t size)
{
   simple_mtx_lock(&device->as.lock);
   util_vma_heap_free(heap, address, size);
   simple_mtx_unlock(&device->as.lock);
}

struct vk_queue;
struct vk_queue_submit;

VkResult panvk_create_bind_queue(struct panvk_device *dev,
                                 const VkDeviceQueueCreateInfo *create_info,
                                 uint32_t queue_idx,
                                 struct vk_queue **out_queue);
void panvk_destroy_bind_queue(struct vk_queue *vk_queue);
VkResult panvk_bind_queue_submit(struct vk_queue *vk_queue,
                                 struct vk_queue_submit *vk_submit);
VkResult panvk_bind_queue_check_status(struct vk_queue *vk_queue);
VkResult panvk_queue_vm_bind(struct vk_queue *vk_queue,
                             struct vk_queue_submit *vk_submit,
                             uint32_t syncobj_handle);

#if PAN_ARCH
VkResult
panvk_per_arch(create_device)(struct panvk_physical_device *physical_device,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice);

void panvk_per_arch(destroy_device)(struct panvk_device *device,
                                    const VkAllocationCallbacks *pAllocator);

/* The gpu_prerast arena (tess = false) or the tessellation arena,
 * allocated on first use on v10+ (csf-v11/205). Returns a zero
 * panvk_priv_mem if the device does not use it or the allocation failed. */
struct panvk_priv_mem
panvk_per_arch(device_gpu_prerast_arena)(struct panvk_device *dev, bool tess);

/* The second tessellation arena (csf-v11 tess-arenas), allocated on first
 * use like the first one. Zero when tessellation is off, PANVK_TESS_ARENAS
 * is 1 or the allocation failed: the draw then uses the first arena. */
struct panvk_priv_mem
panvk_per_arch(device_gpu_prerast_tess_arena2)(struct panvk_device *dev);

static inline VkResult
panvk_common_check_status(struct panvk_device *dev)
{
   return vk_check_printf_status(&dev->vk, &dev->printf.ctx);
}

VkResult panvk_per_arch(device_check_status)(struct vk_device *vk_dev);

#if PAN_ARCH >= 10
VkResult panvk_per_arch(init_tiler_oom)(struct panvk_device *device);
#endif
#endif

#endif
