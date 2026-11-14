/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_BUFFER_H
#define PANVK_BUFFER_H

#include <stdint.h>

#include "vk_buffer.h"

struct panvk_priv_bo;
struct pan_kmod_bo;

struct panvk_buffer {
   struct vk_buffer vk;

   /* Real BO backing a sparse buffer on backends without native sparse VM
    * ops (kbase).  NULL for non-sparse buffers and for backends that support
    * PAN_KMOD_VM_OP_OP_MAP_SPARSE natively.
    */
   struct pan_kmod_bo *sparse_backing;

   /* Whether sparse_backing was fully committed and CPU-mappable. */
   bool sparse_backing_full;

   /* Diagnostic: BO backing this buffer and the offset inside it. */
   struct pan_kmod_bo *diag_bo;
   uint64_t diag_mem_off;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

/* Drop sparse readback-mirror records referencing this buffer and/or memory. */
void panvk_sparse_mirror_forget(VkBuffer buffer, VkDeviceMemory mem);
void panvk_sparse_mirror_upload(void);
void panvk_sparse_mirror_probe(void);
void panvk_import_probe_tick(void);
void panvk_diag_note_rt_draw(VkImage img);
unsigned long long panvk_diag_rt_other_draws(void);
void panvk_diag_note_vbo(VkBuffer buf);
void panvk_diag_note_draw(uint64_t vb_addr, uint32_t base, uint32_t count,
                          uint32_t instances);

static inline uint64_t
panvk_buffer_gpu_ptr(const struct panvk_buffer *buffer, uint64_t offset)
{
   if (!buffer->vk.device_address)
      return 0;

   return vk_buffer_address(&buffer->vk, offset);
}

static inline uint64_t
panvk_buffer_range(const struct panvk_buffer *buffer, uint64_t offset,
                   uint64_t range)
{
   if (!buffer->vk.device_address)
      return 0;

   return vk_buffer_range(&buffer->vk, offset, range);
}

#endif
