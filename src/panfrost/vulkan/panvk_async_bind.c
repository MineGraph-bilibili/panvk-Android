/*
 * Copyright © 2026 NXP
 *
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_buffer.h"
#include "panvk_device.h"
#include "panvk_device_memory.h"
#include "panvk_image.h"
#include "panvk_macros.h"
#include <sys/mman.h>

/* --- kbase sparse readback mirror -------------------------------------
 * kbase cannot remap pages into a sparse buffer's GPU VA, so GPU writes to
 * a sparse buffer land in the backing BO while the app reads the bound
 * memory through its own CPU mapping. Keep a bind table and periodically
 * copy the backing windows back into the bound memory so CPU readback of
 * GPU-finished data sees the real pixels.
 */
struct sparse_mirror_rec {
   VkBuffer buf;
   VkDeviceMemory mem;
   uint64_t res_off;
   uint64_t mem_off;
   uint64_t size;
};

#define SPARSE_MIRROR_MAX 8192
static struct sparse_mirror_rec sparse_mirror_recs[SPARSE_MIRROR_MAX];
static uint32_t sparse_mirror_rec_count;
static uint32_t sparse_mirror_cursor;
static uint64_t sparse_mirror_bytes;
static uint32_t sparse_mirror_submits;
static uint64_t sparse_mirror_up_bytes;
static uint64_t sparse_mirror_down_bytes;
static uint32_t sparse_mirror_syncs;

static void sparse_mirror_sync_bind(VkBuffer buf, VkDeviceMemory mem,
                                    uint64_t res_off, uint64_t mem_off,
                                    uint64_t size);

void
panvk_sparse_mirror_forget(VkBuffer buf, VkDeviceMemory mem)
{
   for (uint32_t i = 0; i < sparse_mirror_rec_count;) {
      if ((buf != VK_NULL_HANDLE && sparse_mirror_recs[i].buf == buf) ||
          (mem != VK_NULL_HANDLE && sparse_mirror_recs[i].mem == mem))
         sparse_mirror_recs[i] = sparse_mirror_recs[--sparse_mirror_rec_count];
      else
         i++;
   }
}

static void
sparse_mirror_record(VkBuffer buf, VkDeviceMemory mem, uint64_t res_off,
                     uint64_t mem_off, uint64_t size)
{
   uint32_t i;
   for (i = 0; i < sparse_mirror_rec_count; i++) {
      if (sparse_mirror_recs[i].buf == buf &&
          sparse_mirror_recs[i].res_off == res_off) {
         sparse_mirror_recs[i].mem = mem;
         sparse_mirror_recs[i].mem_off = mem_off;
         sparse_mirror_recs[i].size = size;
         return;
      }
   }
   if (sparse_mirror_rec_count >= SPARSE_MIRROR_MAX)
      return;
   sparse_mirror_recs[i] = (struct sparse_mirror_rec) {
      .buf = buf, .mem = mem, .res_off = res_off,
      .mem_off = mem_off, .size = size,
   };
   sparse_mirror_rec_count++;
}

static uint64_t
sparse_mirror_run(uint64_t budget)
{
   uint64_t copied = 0;

   if (sparse_mirror_rec_count == 0)
      return 0;

   for (uint32_t n = 0; n < sparse_mirror_rec_count && copied < budget; n++) {
      struct sparse_mirror_rec *r =
         &sparse_mirror_recs[sparse_mirror_cursor++ % sparse_mirror_rec_count];
      VK_FROM_HANDLE(panvk_buffer, b, r->buf);
      VK_FROM_HANDLE(panvk_device_memory, m, r->mem);

      if (b->sparse_backing == NULL || !b->sparse_backing_full)
         continue;
      if (m->addr.host == NULL)
         continue;

      void *bhost = pan_kmod_bo_mmap(b->sparse_backing, PROT_READ,
                                     MAP_SHARED, NULL);
      if (bhost == NULL || bhost == MAP_FAILED)
         continue;

      uint64_t nbytes = MIN2(r->size, budget - copied);
      memcpy((char *)m->addr.host + r->mem_off,
             (char *)bhost + r->res_off, nbytes);
      pan_kmod_bo_munmap(b->sparse_backing, bhost, 0);
      copied += nbytes;
   }
   sparse_mirror_bytes += copied;
   return copied;
}

/* --- CPU -> backing upload ---------------------------------------------
 * The app writes guest memory through its CPU mapping and expects the GPU
 * to see it.  kbase cannot alias those pages into the sparse buffer, so
 * refresh the backing windows right before every GPU submission.
 */
static void
sparse_mirror_cache_clean(const void *start, size_t size)
{
#if defined(__aarch64__)
   uintptr_t ptr = (uintptr_t)start & ~(uintptr_t)63;
   uintptr_t end = ((uintptr_t)start + size + 63) & ~(uintptr_t)63;

   for (; ptr < end; ptr += 64)
      __asm__ volatile("dc cvac, %0" : : "r"(ptr) : "memory");
   __asm__ volatile("dsb sy" ::: "memory");
#else
   (void)start;
   (void)size;
#endif
}

void
panvk_sparse_mirror_upload(void)
{
   static uint32_t upload_cursor;
   static uint64_t upload_submits;
   static uint64_t upload_bytes;
   const uint64_t budget = 64ull << 20;
   uint64_t copied = 0;
   uint64_t rec_bytes = 0;

   if (sparse_mirror_rec_count == 0)
      return;

   for (uint32_t n = 0; n < sparse_mirror_rec_count && copied < budget; n++) {
      struct sparse_mirror_rec *r =
         &sparse_mirror_recs[upload_cursor++ % sparse_mirror_rec_count];
      VK_FROM_HANDLE(panvk_buffer, b, r->buf);
      VK_FROM_HANDLE(panvk_device_memory, m, r->mem);

      rec_bytes += r->size;

      if (b->sparse_backing == NULL || !b->sparse_backing_full)
         continue;
      if (m->addr.host == NULL)
         continue;

      void *bhost = pan_kmod_bo_mmap(b->sparse_backing,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
      if (bhost == NULL || bhost == MAP_FAILED)
         continue;

      uint64_t nbytes = MIN2(r->size, budget - copied);
      memcpy((char *)bhost + r->res_off,
             (const char *)m->addr.host + r->mem_off, nbytes);
      sparse_mirror_cache_clean((char *)bhost + r->res_off, nbytes);
      copied += nbytes;
   }

   upload_bytes += copied;
   if ((++upload_submits % 64) == 0)
      PANVK_DIAG_PRINTF("[PANVK-DIAG] upload stats: submits=%llu recs=%u "
              "rec_bytes=%llu MiB up_total=%llu MiB\n",
              (unsigned long long)upload_submits, sparse_mirror_rec_count,
              (unsigned long long)(rec_bytes >> 20),
              (unsigned long long)(upload_bytes >> 20));
}
#include <string.h>
#include "pan_layout.h"

#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_queue.h"

struct panvk_async_bind_sync_ops {
   struct pan_kmod_sync_op *all;
   size_t all_count;

   struct pan_kmod_sync_op *waits;
   size_t wait_count;

   struct pan_kmod_sync_op *signals;
   size_t signal_count;

   struct pan_kmod_sync_op small_storage[4];
};

static void
panvk_async_bind_sync_ops_init(
   struct panvk_async_bind_sync_ops *sync_ops,
   const struct vk_queue_submit *vk_submit,
   const struct pan_kmod_sync_op *extra_signal)
{
   size_t signal_count = vk_submit->signal_count;
   if (extra_signal)
      signal_count++;

   size_t all_count = vk_submit->wait_count + signal_count;

   sync_ops->all = all_count <= ARRAY_SIZE(sync_ops->small_storage)
                      ? sync_ops->small_storage
                      : malloc(sizeof(*sync_ops->all) * all_count);
   sync_ops->all_count = all_count;

   sync_ops->waits = sync_ops->all;
   sync_ops->wait_count = vk_submit->wait_count;

   sync_ops->signals = sync_ops->all + vk_submit->wait_count;
   sync_ops->signal_count = signal_count;

   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      const struct vk_sync_wait *wait = &vk_submit->waits[i];
      const struct vk_drm_syncobj *syncobj = vk_sync_as_drm_syncobj(wait->sync);
      assert(syncobj);

      sync_ops->waits[i] = (struct pan_kmod_sync_op){
         .handle = syncobj->syncobj,
         .point = wait->wait_value,
      };
   }

   uint32_t signal_idx = 0;
   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      const struct vk_sync_signal *signal = &vk_submit->signals[i];
      const struct vk_drm_syncobj *syncobj =
         vk_sync_as_drm_syncobj(signal->sync);
      assert(syncobj);

      sync_ops->signals[signal_idx++] = (struct pan_kmod_sync_op){
         .handle = syncobj->syncobj,
         .point = signal->signal_value,
      };
   }
   if (extra_signal)
      sync_ops->signals[signal_idx++] = *extra_signal;
   assert(signal_idx == signal_count);
}

static void
panvk_async_bind_sync_ops_cleanup(
   struct panvk_async_bind_sync_ops *sync_ops)
{
   if (sync_ops->all != sync_ops->small_storage)
      free(sync_ops->all);
}

struct panvk_async_bind {
   struct panvk_device *device;
   uint32_t syncobj_handle;

   bool force_sync;

   struct panvk_async_bind_sync_ops sync_ops;
   struct pan_kmod_vm_op storage[16];
   struct pan_kmod_vm_op pending_op;

   struct pan_kmod_vm_multi_op_ctx ctx;
};

static void
panvk_async_bind_init(struct panvk_async_bind *submit,
                             struct panvk_device *device,
                             uint32_t syncobj_handle,
                             struct vk_queue_submit *vk_submit)
{
   const bool force_sync = PANVK_DEBUG(SYNC);

   *submit = (struct panvk_async_bind){
      .device = device,
      .syncobj_handle = syncobj_handle,
      .force_sync = force_sync,
   };

   struct pan_kmod_sync_op syncobj_signal = {
      .handle = syncobj_handle,
      .point = 0,
   };

   panvk_async_bind_sync_ops_init(&submit->sync_ops, vk_submit,
                                         force_sync ? &syncobj_signal : NULL);

   pan_kmod_vm_multi_op_init(&submit->ctx, device->kmod.vm,
                             PAN_KMOD_VM_OP_MODE_ASYNC, submit->storage,
                             ARRAY_SIZE(submit->storage), NULL);

   /* The pending op always starts as a SYNC_ONLY op with just the waits
    * and will be upgraded to a MAP when bind operations are processed.
    */
   submit->pending_op = (struct pan_kmod_vm_op){
      .type = PAN_KMOD_VM_OP_TYPE_SYNC_ONLY,
      .wait = {
         .count = submit->sync_ops.wait_count,
         .array = submit->sync_ops.waits,
      },
   };
}

static void
panvk_async_bind_cleanup(struct panvk_async_bind *submit)
{
   panvk_async_bind_sync_ops_cleanup(&submit->sync_ops);
}

static int
panvk_async_bind_process_signals(struct panvk_async_bind *submit)
{
   struct panvk_device *device = submit->device;
   struct pan_kmod_vm_multi_op_ctx *ctx = &submit->ctx;

   submit->pending_op.signal.array = submit->sync_ops.signals;
   submit->pending_op.signal.count = submit->sync_ops.signal_count;



   /* A SYNC_ONLY op without wait or signal syncs is a no-op. Evict such
    * ops here rather than letting the kernel reject an empty SYNC_ONLY op
    * with -EINVAL in async mode.
    */
   if (submit->pending_op.type == PAN_KMOD_VM_OP_TYPE_SYNC_ONLY &&
       submit->pending_op.wait.count == 0 &&
       submit->pending_op.signal.count == 0)
      return 0;

   int ret = pan_kmod_vm_multi_op_push(ctx, &submit->pending_op);
   if (ret)
      return ret;

   ret = pan_kmod_vm_multi_op_flush(ctx);
   if (ret)
      return ret;

   if (submit->force_sync) {
      ASSERTED int ret = drmSyncobjWait(device->drm_fd,
                               &submit->syncobj_handle, 1, INT64_MAX,
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
      assert(!ret);

      drmSyncobjReset(device->drm_fd, &submit->syncobj_handle, 1);
   }

   return 0;
}

static bool
can_merge_map_ops(struct pan_kmod_vm_op a, struct pan_kmod_vm_op b,
                  struct pan_kmod_vm_op *out)
{
   assert(a.type == PAN_KMOD_VM_OP_TYPE_MAP);
   assert(b.type == PAN_KMOD_VM_OP_TYPE_MAP);

   /* If the flags don't match, we don't merge. */
   if (a.flags != b.flags)
      return false;

   if (b.va.start < a.va.start)
      SWAP(a, b);

   if (a.va.start + a.va.size != b.va.start)
      return false;

   if (!(a.flags & PAN_KMOD_VM_OP_OP_MAP_SPARSE) &&
       (a.map.bo != b.map.bo || a.map.bo_offset + a.va.size != b.map.bo_offset))
      return false;

   *out = (struct pan_kmod_vm_op){
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = a.va.start,
         .size = b.va.start + b.va.size - a.va.start,
      },
      .map = {
         .bo = a.map.bo,
         .bo_offset = a.map.bo_offset,
      },
      .flags = a.flags,
   };
   return true;
}

static int
panvk_async_bind_sparse_memory_bind(
   struct panvk_async_bind *submit,
   VkDeviceAddress resource_va, const VkSparseMemoryBind *in)
{
   VK_FROM_HANDLE(panvk_device_memory, mem, in->memory);
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = resource_va + in->resourceOffset,
         .size = in->size,
      },
   };

   if (in->memory) {
      op.map.bo = mem->bo;
      op.map.bo_offset = in->memoryOffset;
   } else {
      op.flags = PAN_KMOD_VM_OP_OP_MAP_SPARSE;
   }

   /* We can always merge a MAP op with a SYNC_ONLY one. */
   if (submit->pending_op.type == PAN_KMOD_VM_OP_TYPE_SYNC_ONLY ||
       can_merge_map_ops(op, submit->pending_op, &op)) {
      submit->pending_op.type = op.type;
      submit->pending_op.va = op.va;
      submit->pending_op.map = op.map;
      submit->pending_op.flags = op.flags;
      return 0;
   }

   /* If we can't merge, flush the pending op and replace it by our new MAP. */
   int ret = pan_kmod_vm_multi_op_push(&submit->ctx, &submit->pending_op);

   submit->pending_op = op;

   return ret;
}

struct panvk_sparse_block_memory_bind {
   uint32_t plane_index;
   uint32_t layer;
   uint32_t level;
   VkOffset3D offset; /* must be a multiple of extent */
   VkExtent3D extent;
   VkDeviceMemory mem;
   VkDeviceSize mem_offset;
};

static VkSparseMemoryBind
image_to_mem_bind(const struct panvk_image *image,
                  const struct panvk_sparse_block_memory_bind *in)
{
   const struct panvk_image_plane *plane = &image->planes[in->plane_index];

   /* Previously, sparse residency was implemented using block U-interleaved
    * (https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/37483).
    * Interleaved 64k offers better map and unmap performance (at most one bind
    * op per tile, as opposed to 4 to 16 with U-interleaved), and no worse
    * access performance.
    *
    * Note that interleaved 64k is not a thing on pre-v10 hardware. If at any
    * point there's a desire for sparse residency on pre-v10 hardware, resurrect
    * the code linked above.
    */
   assert(image->vk.drm_format_mod == DRM_FORMAT_MOD_ARM_INTERLEAVED_64K);

   struct pan_image_block_size tile_extent_el =
      pan_interleaved_64k_tile_size_el(vk_format_to_pipe_format(image->vk.format));
   struct pan_image_block_size tile_extent_px = {
      tile_extent_el.width * vk_format_get_blockwidth(image->vk.format),
      tile_extent_el.height * vk_format_get_blockheight(image->vk.format),
   };
   VkOffset3D offset_tiles = {
      in->offset.x / tile_extent_px.width,
      in->offset.y / tile_extent_px.height,
      in->offset.z,
   };
   assert(in->extent.width == tile_extent_px.width &&
          in->extent.height == tile_extent_px.height &&
          in->extent.depth == 1);
   uint32_t tile_size_B = 65536;

   assert(image->vk.image_type != VK_IMAGE_TYPE_3D || in->layer == 0);

   const struct pan_image_slice_layout *slayout = &plane->plane.layout.slices[in->level];

   return (VkSparseMemoryBind){
      .resourceOffset =
         in->layer * plane->plane.layout.array_stride_B +
         slayout->offset_B +
         offset_tiles.z * slayout->tiled_or_linear.surface_stride_B +
         offset_tiles.y * slayout->tiled_or_linear.row_stride_B +
         offset_tiles.x * tile_size_B,
      .size = tile_size_B,
      .memory = in->mem,
      .memoryOffset = in->mem_offset,
   };
}

static int
panvk_async_bind_do(struct panvk_async_bind *submit,
                           const struct vk_queue_submit *vk_submit)
{
   int ret;

   for (uint32_t i = 0; i < vk_submit->buffer_bind_count; i++) {
      VK_FROM_HANDLE(panvk_buffer, buf, vk_submit->buffer_binds[i].buffer);
      assert(buf->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT);
      uint64_t resource_va = buf->vk.device_address;

      for (uint32_t j = 0; j < vk_submit->buffer_binds[i].bindCount; j++) {
         const VkSparseMemoryBind *mbind = &vk_submit->buffer_binds[i].pBinds[j];

         ret = panvk_async_bind_sparse_memory_bind(submit, resource_va,
                                                          mbind);
         if (ret)
            return ret;
      }
   }
   for (uint32_t i = 0; i < vk_submit->image_opaque_bind_count; i++) {
      VK_FROM_HANDLE(panvk_image, image,
                     vk_submit->image_opaque_binds[i].image);
      assert(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT);
      uint64_t resource_va = image->sparse.device_address;

      for (uint32_t j = 0; j < vk_submit->image_opaque_binds[i].bindCount;
           j++) {
         const VkSparseMemoryBind *mbind =
            &vk_submit->image_opaque_binds[i].pBinds[j];

         ret = panvk_async_bind_sparse_memory_bind(submit, resource_va,
                                                          mbind);
         if (ret)
            return ret;
      }
   }
   for (uint32_t i = 0; i < vk_submit->image_bind_count; i++) {
      VK_FROM_HANDLE(panvk_image, image, vk_submit->image_binds[i].image);
      assert(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT);
      assert(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT);

      struct panvk_sparse_block_desc sblock = panvk_get_sparse_block_desc(image->vk.image_type, image->vk.format);

      for (uint32_t j = 0; j < vk_submit->image_binds[i].bindCount; j++) {
         const VkSparseImageMemoryBind *ibind =
            &vk_submit->image_binds[i].pBinds[j];

         VkOffset3D max = {
            ibind->offset.x + ibind->extent.width,
            ibind->offset.y + ibind->extent.height,
            ibind->offset.z + ibind->extent.depth,
         };
         struct panvk_sparse_block_memory_bind bind = {
            /* We only support single-plane images right now. See
             * https://gitlab.freedesktop.org/panfrost/mesa/-/issues/243 details. */
            .plane_index = 0,
            .level = ibind->subresource.mipLevel,
            .layer = ibind->subresource.arrayLayer,
            .offset = {},
            .extent = sblock.extent,
            .mem = ibind->memory,
            .mem_offset = ibind->memoryOffset,
         };

         for (bind.offset.z = ibind->offset.z;
              bind.offset.z < max.z; bind.offset.z += sblock.extent.depth) {
            for (bind.offset.y = ibind->offset.y;
                 bind.offset.y < max.y; bind.offset.y += sblock.extent.height) {
               for (bind.offset.x = ibind->offset.x;
                    bind.offset.x < max.x; bind.offset.x += sblock.extent.width) {
                  VkSparseMemoryBind mbind = image_to_mem_bind(image, &bind);

                  ret = panvk_async_bind_sparse_memory_bind(
                     submit, image->sparse.device_address, &mbind);
                  if (ret)
                     return ret;
                  bind.mem_offset += sblock.size_B;
               }
            }
         }
      }
   }

   return panvk_async_bind_process_signals(submit);
}

/* kbase has no DRM syncobjs, no explicit-VA vm_bind and no async VM ops, so a
 * sparse bind submit cannot be executed as a real GPU-side bind.  Sparse
 * buffers are fully backed by a real BO at creation time (see panvk_buffer.c),
 * which makes the binds themselves no-ops; only the sync chain has to be
 * honoured.  Do that synchronously on the CPU.
 */
static VkResult
panvk_queue_vm_bind_kbase(struct vk_queue *vk_queue,
                          struct vk_queue_submit *vk_submit)
{
   struct panvk_device *device = to_panvk_device(vk_queue->base.device);
   VkResult result;

   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      const struct vk_sync_wait *wait = &vk_submit->waits[i];

      result = vk_sync_wait(&device->vk, wait->sync, wait->wait_value,
                            VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(vk_queue, "kbase sparse bind wait: %m");
   }

   uint32_t buffer_bind_count = 0, image_opaque_bind_count = 0,
            image_bind_count = 0;
   for (uint32_t i = 0; i < vk_submit->buffer_bind_count; i++)
      buffer_bind_count += vk_submit->buffer_binds[i].bindCount;
   for (uint32_t i = 0; i < vk_submit->image_opaque_bind_count; i++)
      image_opaque_bind_count += vk_submit->image_opaque_binds[i].bindCount;
   for (uint32_t i = 0; i < vk_submit->image_bind_count; i++)
      image_bind_count += vk_submit->image_binds[i].bindCount;

   static uint32_t bind_submit_log_count;
   if (bind_submit_log_count++ < 16) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] kbase sparse bind submit: buffers=%u/%u "
              "image_opaque=%u/%u images=%u/%u waits=%u signals=%u\n",
              vk_submit->buffer_bind_count, buffer_bind_count,
              vk_submit->image_opaque_bind_count, image_opaque_bind_count,
              vk_submit->image_bind_count, image_bind_count,
              vk_submit->wait_count, vk_submit->signal_count);
   }

   static uint32_t detail_count;
   static uint32_t submit_seq;
   uint64_t total_bytes = 0, min_off = UINT64_MAX, max_end = 0;
   uint32_t mem_count = 0;
   struct panvk_device_memory *seen_mems[8];
   bool log_this = submit_seq < 4 || (submit_seq % 16) == 0;

   submit_seq++;

   for (uint32_t i = 0; i < vk_submit->buffer_bind_count; i++) {
      VK_FROM_HANDLE(panvk_buffer, dbuf, vk_submit->buffer_binds[i].buffer);
      for (uint32_t j = 0; j < vk_submit->buffer_binds[i].bindCount; j++) {
         const VkSparseMemoryBind *mbind = &vk_submit->buffer_binds[i].pBinds[j];
         struct panvk_device_memory *dmem = NULL;
         uint64_t mem_va = 0;
         void *mem_host = NULL;
         uint32_t mem_word = 0;

         if (mbind->memory != VK_NULL_HANDLE) {
            VK_FROM_HANDLE(panvk_device_memory, dmem_i, mbind->memory);
            dmem = dmem_i;
            mem_va = dmem->addr.dev + mbind->memoryOffset;
            if (dmem->addr.host != NULL)
               mem_host = (char *)dmem->addr.host + mbind->memoryOffset;
            if (mem_host != NULL)
               memcpy(&mem_word, mem_host, sizeof(mem_word));
         }

         total_bytes += mbind->size;
         min_off = MIN2(min_off, mbind->resourceOffset);
         max_end = MAX2(max_end, mbind->resourceOffset + mbind->size);

         if (dmem != NULL && mem_count < ARRAY_SIZE(seen_mems)) {
            bool dup = false;
            for (uint32_t k = 0; k < mem_count; k++)
               dup |= seen_mems[k] == dmem;
            if (!dup)
               seen_mems[mem_count++] = dmem;
         }

         if (log_this && detail_count < 8) {
            PANVK_DIAG_PRINTF("[PANVK-DIAG] bind[%u] buf_va=0x%llx off=0x%llx "
                    "size=0x%llx mem_va=0x%llx mem_host=%p mem_word=0x%08x "
                    "full=%d\n",
                    detail_count, (unsigned long long)dbuf->vk.device_address,
                    (unsigned long long)mbind->resourceOffset,
                    (unsigned long long)mbind->size,
                    (unsigned long long)mem_va, mem_host, mem_word,
                    (int)dbuf->sparse_backing_full);
            detail_count++;
         }
      }
   }

   if (log_this) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] bind submit#%u: total=0x%llx "
              "min_off=0x%llx max_end=0x%llx distinct_mem=%u recs=%u "
              "mirror_total=%llu MiB\n",
              submit_seq, (unsigned long long)total_bytes,
              (unsigned long long)(min_off == UINT64_MAX ? 0 : min_off),
              (unsigned long long)max_end, mem_count,
              sparse_mirror_rec_count,
              (unsigned long long)(sparse_mirror_bytes >> 20));
   }

   /* Publish GPU results of the previous binding, then upload the new
    * contents, for exactly the ranges being (re)bound. */
   for (uint32_t i = 0; i < vk_submit->buffer_bind_count; i++) {
      VkBuffer vkbuf = vk_submit->buffer_binds[i].buffer;
      for (uint32_t j = 0; j < vk_submit->buffer_binds[i].bindCount; j++) {
         const VkSparseMemoryBind *mbind = &vk_submit->buffer_binds[i].pBinds[j];
         if (mbind->memory != VK_NULL_HANDLE && mbind->size > 0)
            sparse_mirror_sync_bind(vkbuf, mbind->memory, mbind->resourceOffset,
                                    mbind->memoryOffset, mbind->size);
      }
   }

   if ((++sparse_mirror_submits % 64) == 0) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] mirror stats: submits=%u recs=%u "
              "total=%llu MiB\n",
              sparse_mirror_submits, sparse_mirror_rec_count,
              (unsigned long long)(sparse_mirror_bytes >> 20));
   }

   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      const struct vk_sync_signal *signal = &vk_submit->signals[i];

      result = vk_sync_signal(&device->vk, signal->sync,
                              signal->signal_value);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(vk_queue, "kbase sparse bind signal: %m");
   }

   return VK_SUCCESS;
}

VkResult
panvk_queue_vm_bind(struct vk_queue *vk_queue,
                    struct vk_queue_submit *vk_submit, uint32_t syncobj_handle)
{
   struct panvk_device *device = to_panvk_device(vk_queue->base.device);
   struct panvk_async_bind submit;
   VkResult result = VK_SUCCESS;

   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);
   if (phys_dev->kbase_node_path[0] != '\0')
      return panvk_queue_vm_bind_kbase(vk_queue, vk_submit);

   panvk_async_bind_init(&submit, device, syncobj_handle, vk_submit);

   int ret = panvk_async_bind_do(&submit, vk_submit);
   if (ret)
      result = vk_queue_set_lost(vk_queue, "GROUP_SUBMIT: %m");

   panvk_async_bind_cleanup(&submit);

   return result;
}

#if defined(__aarch64__)
static void
sparse_mirror_cache_inval(const void *start, size_t size)
{
   uintptr_t ptr = (uintptr_t)start & ~(uintptr_t)63;
   uintptr_t end = ((uintptr_t)start + size + 63) & ~(uintptr_t)63;

   for (; ptr < end; ptr += 64)
      __asm__ volatile("dc civac, %0" : : "r"(ptr) : "memory");
   __asm__ volatile("dsb sy" ::: "memory");
}
#else
static void
sparse_mirror_cache_inval(const void *start, size_t size)
{
   (void)start;
   (void)size;
}
#endif

static void
sparse_mirror_copy_range(VkBuffer buf, VkDeviceMemory mem, uint64_t res_off,
                         uint64_t mem_off, uint64_t size, bool to_backing)
{
   VK_FROM_HANDLE(panvk_buffer, b, buf);
   VK_FROM_HANDLE(panvk_device_memory, m, mem);

   if (b == NULL || m == NULL || size == 0 ||
       b->sparse_backing == NULL || !b->sparse_backing_full ||
       m->addr.host == NULL)
      return;

   void *bhost = pan_kmod_bo_mmap(b->sparse_backing, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, NULL);
   if (bhost == NULL || bhost == MAP_FAILED)
      return;

   char *ahost = (char *)bhost + res_off;
   char *mhost = (char *)m->addr.host + mem_off;

   if (to_backing) {
      memcpy(ahost, mhost, size);
      sparse_mirror_cache_clean(ahost, size);
      sparse_mirror_up_bytes += size;
   } else {
      sparse_mirror_cache_inval(ahost, size);
      memcpy(mhost, ahost, size);
      sparse_mirror_down_bytes += size;
   }

   pan_kmod_bo_munmap(b->sparse_backing, bhost, 0);
}

static void
sparse_mirror_sync_bind(VkBuffer buf, VkDeviceMemory mem, uint64_t res_off,
                        uint64_t mem_off, uint64_t size)
{
   /* The GPU may have written the previously bound window: publish it back
    * to that memory before its contents are replaced. */
   for (uint32_t i = 0; i < sparse_mirror_rec_count; i++) {
      struct sparse_mirror_rec *r = &sparse_mirror_recs[i];

      if (r->buf == buf && r->res_off == res_off && r->mem != mem)
         sparse_mirror_copy_range(buf, r->mem, res_off, r->mem_off,
                                  MIN2(r->size, size), false);
   }

   sparse_mirror_record(buf, mem, res_off, mem_off, size);
   sparse_mirror_copy_range(buf, mem, res_off, mem_off, size, true);

   if ((++sparse_mirror_syncs % 256) == 0)
      PANVK_DIAG_PRINTF("[PANVK-DIAG] sparse sync: binds=%u up=%llu MiB "
                        "down=%llu MiB recs=%u\n",
                        sparse_mirror_syncs,
                        (unsigned long long)(sparse_mirror_up_bytes >> 20),
                        (unsigned long long)(sparse_mirror_down_bytes >> 20),
                        sparse_mirror_rec_count);
}

static void
sparse_probe_words(const void *base, uint64_t offset, uint64_t size,
                   uint64_t *nz, uint64_t *hash)
{
   const uint32_t *p = (const uint32_t *)((const char *)base + offset);
   const uint64_t nwords = MIN2(size, (uint64_t)16384);

   *nz = 0;
   *hash = 1469598103934665603ull;
   for (uint64_t i = 0; i < nwords; i++) {
      *nz += (p[i] != 0);
      *hash = (*hash ^ p[i]) * 1099511628211ull;
   }
}

void
panvk_sparse_mirror_probe(void)
{
   static uint32_t probe_ticks;

   if ((probe_ticks++ % 8) != 0 || sparse_mirror_rec_count == 0)
      return;

   /* Scan the first 4 KiB of every bound range and report whether any of it
    * ever receives data. */
   {
      VkBuffer cur_buf = VK_NULL_HANDLE;
      struct pan_kmod_bo *cur_bo = NULL;
      void *cur_host = NULL;
      uint64_t recs_seen = 0, bytes_seen = 0, total_nz = 0;

      for (uint32_t i = 0; i < sparse_mirror_rec_count; i++) {
         struct sparse_mirror_rec *r = &sparse_mirror_recs[i];

         if (r->buf != cur_buf) {
            if (cur_bo != NULL)
               pan_kmod_bo_munmap(cur_bo, cur_host, 0);
            cur_bo = NULL;
            cur_host = NULL;
            cur_buf = r->buf;

            VK_FROM_HANDLE(panvk_buffer, b, cur_buf);
            if (b != NULL && b->sparse_backing != NULL) {
               cur_bo = b->sparse_backing;
               cur_host = pan_kmod_bo_mmap(cur_bo, PROT_READ, MAP_SHARED, NULL);
               if (cur_host == MAP_FAILED)
                  cur_host = NULL;
            }
         }
         if (cur_host == NULL)
            continue;

         uint64_t nz, hash;
         sparse_mirror_cache_inval((char *)cur_host + r->res_off,
                                   MIN2(r->size, (uint64_t)4096));
         sparse_probe_words(cur_host, r->res_off, MIN2(r->size, (uint64_t)4096),
                            &nz, &hash);
         recs_seen++;
         bytes_seen += MIN2(r->size, (uint64_t)4096);
         total_nz += nz;
      }
      if (cur_bo != NULL)
         pan_kmod_bo_munmap(cur_bo, cur_host, 0);

      PANVK_DIAG_PRINTF("[PANVK-DIAG] SPARSE ALL: recs=%llu sampled=%llu KiB "
                        "nz=%llu words\n",
                        (unsigned long long)recs_seen,
                        (unsigned long long)(bytes_seen >> 10),
                        (unsigned long long)total_nz);
   }

   for (uint32_t i = (sparse_mirror_rec_count > 4 ? sparse_mirror_rec_count - 4 : 0);
        i < sparse_mirror_rec_count; i++) {
      struct sparse_mirror_rec *r = &sparse_mirror_recs[i];
      VK_FROM_HANDLE(panvk_buffer, b, r->buf);
      VK_FROM_HANDLE(panvk_device_memory, m, r->mem);

      if (b == NULL || m == NULL || m->addr.host == NULL ||
          b->sparse_backing == NULL)
         continue;

      void *bhost = pan_kmod_bo_mmap(b->sparse_backing, PROT_READ,
                                     MAP_SHARED, NULL);
      if (bhost == NULL || bhost == MAP_FAILED)
         continue;

      sparse_mirror_cache_inval((char *)bhost + r->res_off,
                                MIN2(r->size, (uint64_t)65536));
      sparse_mirror_cache_inval((char *)m->addr.host + r->mem_off,
                                MIN2(r->size, (uint64_t)65536));

      uint64_t mnz, mhash, anz, ahash;
      sparse_probe_words(m->addr.host, r->mem_off, r->size, &mnz, &mhash);
      sparse_probe_words(bhost, r->res_off, r->size, &anz, &ahash);

      PANVK_DIAG_PRINTF("[PANVK-DIAG] SPARSE[%u]: off=0x%llx size=0x%llx "
                        "mem nz=%llu hash=%016llx | arena nz=%llu hash=%016llx\n",
                        i, (unsigned long long)r->res_off,
                        (unsigned long long)r->size,
                        (unsigned long long)mnz, (unsigned long long)mhash,
                        (unsigned long long)anz, (unsigned long long)ahash);

      pan_kmod_bo_munmap(b->sparse_backing, bhost, 0);
   }
}
