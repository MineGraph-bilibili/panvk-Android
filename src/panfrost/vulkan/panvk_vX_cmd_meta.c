/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "util/log.h"
#include "util/u_debug.h"

#include "panvk_buffer.h"
#include "panvk_cmd_alloc.h"
#include "panvk_cmd_meta.h"
#include "panvk_entrypoints.h"
#include "panvk_meta.h"
#include "panvk_tracepoints.h"
#if PAN_ARCH >= 10
#include "csf/panvk_instr.h"
#endif

#include "util/u_atomic.h"

#include "bc/panvk_bc_s3tc_astc_spv.h"
#include "bc/panvk_bc_bc7_astc_spv.h"
#include "bc/panvk_bc_bc6_astc_spv.h"
#include "bc/panvk_bc_rgtc_snorm_spv.h"
#include "bc/panvk_bc_copy_spv.h"
#include "bc/panvk_bc_rgtc_eac_spv.h"
#include "bc/panvk_bc_s3tc_etc_spv.h"
#include "bc/panvk_bc_alpha_fs_spv.h"

#include "panvk_cmd_precomp.h"
#include "libpan.h"
#include "libpan_copy.h"
#include "libpan_dgc.h"

static bool
copy_to_image_use_gfx_pipeline(struct panvk_image *dst_img)
{
   /* Don't force gfx-based copies if the format is bigger than 32-bit. */
   if (PANVK_DEBUG(COPY_GFX) &&
       vk_format_get_blocksize(dst_img->vk.format) <= 4)
      return true;

   /* Writes to AFBC images must go through the graphics pipeline. */
   if (drm_is_afbc(dst_img->vk.drm_format_mod))
      return true;

   return false;
}

#if PAN_ARCH >= 10
/* Compute copies bypass framebuffer CRC tracking, so writes touching the
 * mip-0 CRC region must invalidate its state explicitly.
 */
static bool
copy_buffer_to_image_touches_crc(const struct panvk_image *dst_img,
                                 const VkCopyDeviceMemoryImageInfoKHR *info)
{
   if (!panvk_image_plane_crc_header_addr(dst_img))
      return false;

   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkImageSubresourceLayers *dst = &info->pRegions[i].imageSubresource;

      if (dst->mipLevel == 0 &&
          panvk_plane_index(dst_img, dst->aspectMask) == 0)
         return true;
   }

   return false;
}

static bool
copy_image_touches_crc(const struct panvk_image *dst_img,
                       const VkCopyImageInfo2 *info)
{
   if (!panvk_image_plane_crc_header_addr(dst_img))
      return false;

   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkImageSubresourceLayers *dst = &info->pRegions[i].dstSubresource;

      if (dst->mipLevel == 0 &&
          panvk_plane_index(dst_img, dst->aspectMask) == 0)
         return true;
   }

   return false;
}
#endif /* PAN_ARCH >= 10 */

static void
meta_compute_start(struct panvk_cmd_buffer *cmdbuf,
                   struct panvk_cmd_meta_compute_save_ctx *save_ctx)
{
   const struct panvk_descriptor_set *set0 =
      cmdbuf->state.compute.desc_state.sets[0];
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.compute.desc_state.push_sets[0];

   save_ctx->set0 = set0;
   if (push_set0 && push_set0 == set0) {
      save_ctx->push_set0.desc_count = push_set0->desc_count;
      save_ctx->push_set0.descs_dev_addr = push_set0->descs.dev;
      save_ctx->push_set0.dirty =
         BITSET_TEST(cmdbuf->state.compute.desc_state.dirty_push_sets, 0);
      memcpy(save_ctx->push_set0.desc_storage, push_set0->descs.host,
             push_set0->desc_count * PANVK_DESCRIPTOR_SIZE);
   }

   save_ctx->push_constants = cmdbuf->state.push_constants;
   save_ctx->cs.shader = cmdbuf->state.compute.shader;
   save_ctx->cs.desc = cmdbuf->state.compute.cs.desc;
#if PAN_ARCH >= 10
   save_ctx->cond_render_enabled = cmdbuf->state.cond_render.enabled;
   save_ctx->cond_render_inherited = cmdbuf->state.cond_render.inherited;
   cmdbuf->state.cond_render.enabled = false;
   cmdbuf->state.cond_render.inherited = false;
#endif

#if PAN_ARCH >= 10
   panvk_per_arch(panvk_instr_begin_work)(PANVK_SUBQUEUE_COMPUTE, cmdbuf,
                                          PANVK_INSTR_WORK_TYPE_META);
#endif
}

static void
meta_compute_end(struct panvk_cmd_buffer *cmdbuf,
                 const struct panvk_cmd_meta_compute_save_ctx *save_ctx)
{
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.compute.desc_state.push_sets[0];

#if PAN_ARCH >= 10
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   panvk_per_arch(panvk_instr_end_work_async)(
      PANVK_SUBQUEUE_COMPUTE, cmdbuf, PANVK_INSTR_WORK_TYPE_META, NULL,
      cs_defer(dev->csf.sb.all_iters_mask, 0));
#endif

   cmdbuf->state.compute.desc_state.sets[0] = save_ctx->set0;
   if (save_ctx->push_set0.desc_count) {
      memcpy(push_set0->descs.host, save_ctx->push_set0.desc_storage,
             save_ctx->push_set0.desc_count * PANVK_DESCRIPTOR_SIZE);
      push_set0->descs.dev = save_ctx->push_set0.descs_dev_addr;
      push_set0->desc_count = save_ctx->push_set0.desc_count;

      if (save_ctx->push_set0.dirty)
         BITSET_SET(cmdbuf->state.compute.desc_state.dirty_push_sets, 0);
      else
         BITSET_CLEAR(cmdbuf->state.compute.desc_state.dirty_push_sets, 0);
   }

   cmdbuf->state.push_constants = save_ctx->push_constants;
   compute_state_set_dirty(cmdbuf, PUSH_UNIFORMS);

   cmdbuf->state.compute.shader = save_ctx->cs.shader;
   cmdbuf->state.compute.cs.desc = save_ctx->cs.desc;
#if PAN_ARCH >= 10
   cmdbuf->state.cond_render.enabled = save_ctx->cond_render_enabled;
   cmdbuf->state.cond_render.inherited = save_ctx->cond_render_inherited;
#endif
   compute_state_set_dirty(cmdbuf, CS);
   compute_state_set_dirty(cmdbuf, DESC_STATE);
}

static void
meta_gfx_start(struct panvk_cmd_buffer *cmdbuf,
               struct panvk_cmd_meta_graphics_save_ctx *save_ctx)
{
   const struct panvk_descriptor_set *set0 =
      cmdbuf->state.gfx.desc_state.sets[0];
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.gfx.desc_state.push_sets[0];

   save_ctx->set0 = set0;
   if (push_set0 && push_set0 == set0) {
      save_ctx->push_set0.desc_count = push_set0->desc_count;
      save_ctx->push_set0.descs_dev_addr = push_set0->descs.dev;
      save_ctx->push_set0.dirty =
         BITSET_TEST(cmdbuf->state.gfx.desc_state.dirty_push_sets, 0);
      memcpy(save_ctx->push_set0.desc_storage, push_set0->descs.host,
             push_set0->desc_count * PANVK_DESCRIPTOR_SIZE);
   }

   save_ctx->push_constants = cmdbuf->state.push_constants;
   save_ctx->fs.shader = cmdbuf->state.gfx.fs.shader;
   save_ctx->fs.desc = cmdbuf->state.gfx.fs.desc;
   save_ctx->vs.shader = cmdbuf->state.gfx.vs.shader;
   save_ctx->vs.desc = cmdbuf->state.gfx.vs.desc;
   save_ctx->vb0 = cmdbuf->state.gfx.vb.bufs[0];
#if PAN_ARCH >= 9
   save_ctx->gs = cmdbuf->state.gfx.gs.shader;
   save_ctx->tcs = cmdbuf->state.gfx.tcs.shader;
   save_ctx->tes = cmdbuf->state.gfx.tes.shader;
   cmdbuf->state.gfx.gs.shader = NULL;
   cmdbuf->state.gfx.tcs.shader = NULL;
   cmdbuf->state.gfx.tes.shader = NULL;
#endif

   save_ctx->dyn_state.all = cmdbuf->vk.dynamic_graphics_state;
   save_ctx->dyn_state.vi = cmdbuf->state.gfx.dynamic.vi;
   save_ctx->dyn_state.sl = cmdbuf->state.gfx.dynamic.sl;
   save_ctx->occlusion_query = cmdbuf->state.gfx.occlusion_query;

   /* Ensure occlusion queries are disabled */
   cmdbuf->state.gfx.occlusion_query.ptr = 0;
   cmdbuf->state.gfx.occlusion_query.mode = MALI_OCCLUSION_MODE_DISABLED;
   gfx_state_set_dirty(cmdbuf, OQ);

   cmdbuf->state.gfx.vk_meta = true;
#if PAN_ARCH >= 10
   save_ctx->cond_render_enabled = cmdbuf->state.cond_render.enabled;
   save_ctx->cond_render_inherited = cmdbuf->state.cond_render.inherited;
   cmdbuf->state.cond_render.enabled = false;
   cmdbuf->state.cond_render.inherited = false;
#endif

#if PAN_ARCH >= 10
   panvk_per_arch(panvk_instr_begin_work)(PANVK_SUBQUEUE_VERTEX_TILER, cmdbuf,
                                          PANVK_INSTR_WORK_TYPE_META);
   panvk_per_arch(panvk_instr_begin_work)(PANVK_SUBQUEUE_FRAGMENT, cmdbuf,
                                          PANVK_INSTR_WORK_TYPE_META);
#endif
}

static void
meta_gfx_end(struct panvk_cmd_buffer *cmdbuf,
             const struct panvk_cmd_meta_graphics_save_ctx *save_ctx)
{
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.gfx.desc_state.push_sets[0];

#if PAN_ARCH >= 10
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   panvk_per_arch(panvk_instr_end_work_async)(
      PANVK_SUBQUEUE_VERTEX_TILER, cmdbuf, PANVK_INSTR_WORK_TYPE_META, NULL,
      cs_defer(dev->csf.sb.all_iters_mask, 0));
   panvk_per_arch(panvk_instr_end_work_async)(
      PANVK_SUBQUEUE_FRAGMENT, cmdbuf, PANVK_INSTR_WORK_TYPE_META, NULL,
      cs_defer(dev->csf.sb.all_iters_mask, 0));
#endif

   cmdbuf->state.gfx.desc_state.sets[0] = save_ctx->set0;
   if (save_ctx->push_set0.desc_count) {
      memcpy(push_set0->descs.host, save_ctx->push_set0.desc_storage,
             save_ctx->push_set0.desc_count * PANVK_DESCRIPTOR_SIZE);
      push_set0->descs.dev = save_ctx->push_set0.descs_dev_addr;
      push_set0->desc_count = save_ctx->push_set0.desc_count;

      if (save_ctx->push_set0.dirty)
         BITSET_SET(cmdbuf->state.gfx.desc_state.dirty_push_sets, 0);
      else
         BITSET_CLEAR(cmdbuf->state.gfx.desc_state.dirty_push_sets, 0);
   }

   cmdbuf->state.push_constants = save_ctx->push_constants;
   gfx_state_set_dirty(cmdbuf, VS_PUSH_UNIFORMS);
   gfx_state_set_dirty(cmdbuf, FS_PUSH_UNIFORMS);

   cmdbuf->state.gfx.fs.shader = save_ctx->fs.shader;
   cmdbuf->state.gfx.fs.desc = save_ctx->fs.desc;
   cmdbuf->state.gfx.vs.shader = save_ctx->vs.shader;
   cmdbuf->state.gfx.vs.desc = save_ctx->vs.desc;
   cmdbuf->state.gfx.vb.bufs[0] = save_ctx->vb0;
#if PAN_ARCH >= 9
   cmdbuf->state.gfx.gs.shader = save_ctx->gs;
   cmdbuf->state.gfx.tcs.shader = save_ctx->tcs;
   cmdbuf->state.gfx.tes.shader = save_ctx->tes;
#endif

#if PAN_ARCH < 9
   cmdbuf->state.gfx.vs.attribs = 0;
   cmdbuf->state.gfx.vs.attrib_bufs = 0;
   cmdbuf->state.gfx.vs.indirect_attribs_infos = 0;
   cmdbuf->state.gfx.vs.indirect_attrib_bufs_infos = 0;
   cmdbuf->state.gfx.vs.indirect_varying_bufs_infos = 0;
   cmdbuf->state.gfx.fs.rsd = 0;
#else
   cmdbuf->state.gfx.fs.desc.res_table = 0;
   cmdbuf->state.gfx.vs.desc.res_table = 0;
#endif

   cmdbuf->vk.dynamic_graphics_state = save_ctx->dyn_state.all;
   cmdbuf->state.gfx.dynamic.vi = save_ctx->dyn_state.vi;
   cmdbuf->state.gfx.dynamic.sl = save_ctx->dyn_state.sl;
   cmdbuf->state.gfx.occlusion_query = save_ctx->occlusion_query;
   memcpy(cmdbuf->vk.dynamic_graphics_state.dirty,
          cmdbuf->vk.dynamic_graphics_state.set,
          sizeof(cmdbuf->vk.dynamic_graphics_state.set));
   gfx_state_set_dirty(cmdbuf, VS);
   gfx_state_set_dirty(cmdbuf, FS);
   gfx_state_set_dirty(cmdbuf, VB);
   gfx_state_set_dirty(cmdbuf, OQ);
   gfx_state_set_dirty(cmdbuf, DESC_STATE);
   gfx_state_set_dirty(cmdbuf, RENDER_STATE);

   cmdbuf->state.gfx.vk_meta = false;
#if PAN_ARCH >= 10
   cmdbuf->state.cond_render.enabled = save_ctx->cond_render_enabled;
   cmdbuf->state.cond_render.inherited = save_ctx->cond_render_inherited;
#endif
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBlitImage2)(VkCommandBuffer commandBuffer,
                              const VkBlitImageInfo2 *pBlitImageInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};

   meta_gfx_start(cmdbuf, &save);
   vk_meta_blit_image2(&cmdbuf->vk, &dev->meta, pBlitImageInfo);
   meta_gfx_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdResolveImage2)(VkCommandBuffer commandBuffer,
                                 const VkResolveImageInfo2 *pResolveImageInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};

   meta_gfx_start(cmdbuf, &save);
   vk_meta_resolve_image2(&cmdbuf->vk, &dev->meta, pResolveImageInfo);
   meta_gfx_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdClearAttachments)(VkCommandBuffer commandBuffer,
                                    uint32_t attachmentCount,
                                    const VkClearAttachment *pAttachments,
                                    uint32_t rectCount,
                                    const VkClearRect *pRects)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};
   struct vk_meta_rendering_info render = {
      .view_mask = cmdbuf->state.gfx.render.view_mask,
      .samples = cmdbuf->state.gfx.render.fb.nr_samples,
      .depth_attachment_format = cmdbuf->state.gfx.render.z_attachment.fmt,
      .stencil_attachment_format = cmdbuf->state.gfx.render.s_attachment.fmt,
   };
   for (uint32_t i = 0; i < MAX_RTS; i++) {
      if (!(cmdbuf->state.gfx.render.bound_attachments &
            MESA_VK_RP_ATTACHMENT_COLOR_BIT(i)))
         continue;

      render.color_attachment_count =
         MAX2(render.color_attachment_count, i + 1);
      render.color_attachment_formats[i] =
         cmdbuf->state.gfx.render.color_attachments.fmts[i];
      render.color_attachment_write_masks[i] =
         VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
   }

   meta_gfx_start(cmdbuf, &save);
#if PAN_ARCH >= 10
   /* CmdClearAttachments IS affected by conditional rendering, so restore
    * the state that meta_gfx_start disabled.
    */
   cmdbuf->state.cond_render.enabled = save.cond_render_enabled;
   cmdbuf->state.cond_render.inherited = save.cond_render_inherited;
#endif
   vk_meta_clear_attachments(&cmdbuf->vk, &dev->meta, &render, attachmentCount,
                             pAttachments, rectCount, pRects);
   meta_gfx_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdClearDepthStencilImage)(
   VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
   const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   VK_FROM_HANDLE(panvk_image, img, image);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};

   meta_gfx_start(cmdbuf, &save);
   vk_meta_clear_depth_stencil_image(&cmdbuf->vk, &dev->meta, &img->vk,
                                     imageLayout, pDepthStencil, rangeCount,
                                     pRanges);
   meta_gfx_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdClearColorImage)(VkCommandBuffer commandBuffer, VkImage image,
                                   VkImageLayout imageLayout,
                                   const VkClearColorValue *pColor,
                                   uint32_t rangeCount,
                                   const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   VK_FROM_HANDLE(panvk_image, img, image);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};

   /* Mali cannot render to R64; alias as RG32UI for vk_meta. */
   VkFormat view_format = img->vk.format;
   if (img->vk.format == VK_FORMAT_R64_UINT ||
       img->vk.format == VK_FORMAT_R64_SINT)
      view_format = VK_FORMAT_R32G32_UINT;

   meta_gfx_start(cmdbuf, &save);
   vk_meta_clear_color_image(&cmdbuf->vk, &dev->meta, &img->vk, imageLayout,
                             view_format, pColor, rangeCount, pRanges);
   meta_gfx_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyMemoryKHR)(VkCommandBuffer commandBuffer,
                                 const VkCopyDeviceMemoryInfoKHR* pCopyMemoryInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_compute_save_ctx save = {0};

   meta_compute_start(cmdbuf, &save);
   vk_meta_copy_memory(&cmdbuf->vk, &dev->meta, pCopyMemoryInfo);
   meta_compute_end(cmdbuf, &save);
}

static bool
lower_copy_memory_to_image(
   VkCommandBuffer commandBuffer,
   const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo)
{
   VK_FROM_HANDLE(panvk_image, dst_img, pCopyMemoryInfo->image);

   const VkImageAspectFlags zs_mask =
      (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   /* Only required for interleaved depth stencil that are not multi-planar */
   if (vk_format_aspects(dst_img->vk.format) != zs_mask ||
       dst_img->plane_count > 1)
      return false;

   uint32_t num_depth_regions = 0, num_stencil_regions = 0;
   for (uint32_t i = 0; i < pCopyMemoryInfo->regionCount; i++) {
      const VkImageAspectFlags aspect_mask =
         pCopyMemoryInfo->pRegions[i].imageSubresource.aspectMask;
      assert((aspect_mask & ~zs_mask) == 0);
      if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
         num_depth_regions++;
      else
         num_stencil_regions++;
   }

   /* If we have both depth and stencil writes to an interleaved depth stencil
    * image, we must split the writes per aspect with a barrier between them to
    * avoid a write-after-write race. */
   const bool lowering_needed = (num_depth_regions && num_stencil_regions);
   if (!lowering_needed)
      return false;

   VkCopyDeviceMemoryImageInfoKHR adjusted_info = *pCopyMemoryInfo;
   STACK_ARRAY(VkDeviceMemoryImageCopyKHR, depth_regions, num_depth_regions);
   STACK_ARRAY(VkDeviceMemoryImageCopyKHR, stencil_regions, num_stencil_regions);

   uint32_t depth_idx = 0, stencil_idx = 0;
   for (uint32_t i = 0; i < pCopyMemoryInfo->regionCount; i++) {
      const VkImageAspectFlags aspect_mask =
         pCopyMemoryInfo->pRegions[i].imageSubresource.aspectMask;

      if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
         depth_regions[depth_idx++] = pCopyMemoryInfo->pRegions[i];
      else
         stencil_regions[stencil_idx++] = pCopyMemoryInfo->pRegions[i];
   }

   adjusted_info.regionCount = num_depth_regions;
   adjusted_info.pRegions = depth_regions;
   panvk_per_arch(CmdCopyMemoryToImageKHR)(commandBuffer, &adjusted_info);

   const VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
   const VkDependencyInfo dep_info = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mem_barrier,
   };
   panvk_per_arch(CmdPipelineBarrier2)(commandBuffer, &dep_info);

   adjusted_info.regionCount = num_stencil_regions;
   adjusted_info.pRegions = stencil_regions;
   panvk_per_arch(CmdCopyMemoryToImageKHR)(commandBuffer, &adjusted_info);

   STACK_ARRAY_FINISH(depth_regions);
   STACK_ARRAY_FINISH(stencil_regions);

   return true;
}

static void copy_memory_to_image_raw(
   VkCommandBuffer commandBuffer,
   const VkCopyDeviceMemoryImageInfoKHR *pCopyMemoryInfo);
static void copy_image_raw(VkCommandBuffer commandBuffer,
                           const VkCopyImageInfo2 *pCopyImageInfo);

struct panvk_bc_decode_push {
   uint32_t src[2];
   uint32_t dst[2];
   uint32_t src_row_B;
   uint32_t src_slice_B;
   uint32_t dst_row_B;
   uint32_t dst_slice_B;
   int32_t width;
   int32_t height;
   int32_t depth;
   int32_t format;
   uint32_t dst_x0;
   uint32_t dst_y0;
   uint32_t dst_tiled; /* panvk_bc_copy.comp: tile size log2 */
   uint32_t src_x0;    /* panvk_bc_copy.comp only */
   uint32_t src_y0;
   uint32_t lut[2];    /* panvk_bc_rgtc_eac.comp: EAC parameter table */
};

struct panvk_bc_decode_key {
   enum panvk_meta_object_key_type type;
   uint32_t shader;
   /* Specialization constant 0: the transcoders fold their per-format path
    * at pipeline creation, which keeps register pressure per format low. */
   int32_t format;
};

/* The BC4/BC5 -> EAC table (256 KiB) is uploaded on first use. */
static uint64_t
panvk_bc_eac_lut_addr(struct panvk_device *dev)
{
   struct panvk_priv_bo *bo = p_atomic_read(&dev->bc_eac_lut);

   if (!bo) {
      const size_t size = panvk_bc_eac_lut_size();

      if (panvk_priv_bo_create(dev, size,
                               panvk_device_adjust_bo_flags(dev, PAN_KMOD_BO_FLAG_WB_MMAP),
                               VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
                               &bo) != VK_SUCCESS)
         return 0;

      memcpy(bo->addr.host, panvk_bc_eac_lut_data(), size);
      panvk_priv_bo_flush(bo, 0, size);

      struct panvk_priv_bo *old = p_atomic_cmpxchg_ptr(&dev->bc_eac_lut, NULL, bo);
      if (old) {
         panvk_priv_bo_unref(bo);
         bo = old;
      }
   }

   return bo->addr.dev;
}

#define BC_COPY_SHADER 4
#define BC_EAC_SHADER 5
#define BC_ETC_SHADER 6

static uint32_t
bc_decode_shader_index(VkFormat format)
{
   if (format >= VK_FORMAT_BC4_UNORM_BLOCK &&
       format <= VK_FORMAT_BC5_SNORM_BLOCK)
      return panvk_bc_eac_enabled() ? BC_EAC_SHADER /* -> EAC R11/RG11 */
                                    : 3; /* -> R8/RG8 */
   if (format <= VK_FORMAT_BC3_SRGB_BLOCK && panvk_bc_etc_enabled())
      return BC_ETC_SHADER; /* BC1 -> ETC2 RGB8/RGB8A1, BC2/BC3 -> RGBA8 */
   if (format <= VK_FORMAT_BC5_SNORM_BLOCK)
      return 0; /* BC1-BC5 -> ASTC LDR */
   if (format <= VK_FORMAT_BC6H_SFLOAT_BLOCK)
      return 2; /* BC6H -> ASTC HDR */
   return 1;    /* BC7 -> ASTC LDR */
}

static VkResult
get_bc_decode_pipeline(struct panvk_device *dev, uint32_t shader, VkFormat format,
                       VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   static const struct {
      const uint32_t *code;
      size_t size;
   } spv[] = {
      {panvk_bc_s3tc_astc_spv, sizeof(panvk_bc_s3tc_astc_spv)},
      {panvk_bc_bc7_astc_spv, sizeof(panvk_bc_bc7_astc_spv)},
      {panvk_bc_bc6_astc_spv, sizeof(panvk_bc_bc6_astc_spv)},
      {panvk_bc_rgtc_snorm_spv, sizeof(panvk_bc_rgtc_snorm_spv)},
      {panvk_bc_copy_spv, sizeof(panvk_bc_copy_spv)},
      {panvk_bc_rgtc_eac_spv, sizeof(panvk_bc_rgtc_eac_spv)},
      {panvk_bc_s3tc_etc_spv, sizeof(panvk_bc_s3tc_etc_spv)},
   };
   const enum panvk_meta_object_key_type layout_key =
      PANVK_META_OBJECT_KEY_BC_DECODE_LAYOUT;
   const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(struct panvk_bc_decode_push),
   };
   VkResult result =
      vk_meta_get_pipeline_layout(&dev->vk, &dev->meta, NULL, &push_range,
                                  &layout_key, sizeof(layout_key), layout_out);
   if (result != VK_SUCCESS)
      return result;

   const struct panvk_bc_decode_key key = {
      .type = PANVK_META_OBJECT_KEY_BC_DECODE_SHADER,
      .shader = shader,
      .format = format,
   };
   VkPipeline cached = vk_meta_lookup_pipeline(&dev->meta, &key, sizeof(key));
   if (cached != VK_NULL_HANDLE) {
      *pipeline_out = cached;
      return VK_SUCCESS;
   }

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spv[shader].size,
      .pCode = spv[shader].code,
   };
   const VkSpecializationMapEntry spec_entry = {
      .constantID = 0,
      .offset = offsetof(struct panvk_bc_decode_key, format),
      .size = sizeof(key.format),
   };
   const VkSpecializationInfo spec_info = {
      .mapEntryCount = 1,
      .pMapEntries = &spec_entry,
      .dataSize = sizeof(key),
      .pData = &key,
   };
   const VkComputePipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = &module_info,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .pName = "main",
         .pSpecializationInfo = &spec_info,
      },
      .layout = *layout_out,
   };

   return vk_meta_create_compute_pipeline(&dev->vk, &dev->meta, &info, &key,
                                          sizeof(key), pipeline_out);
}

/* ---- BC2/BC3 into an AFRC shadow: fragment decode -------------------- */

struct panvk_bc_afrc_push {
   uint32_t src[2];
   uint32_t src_row_B;
   uint32_t src_slice_B;
   int32_t x0;
   int32_t y0;
   int32_t bc2;
};

/* Where block (0, 0) of a decode region lives, and its pitches. */
struct bc_decode_src {
   uint64_t addr;
   uint32_t row_B;
   uint32_t slice_B;
};

static VkResult
get_bc_decode_afrc_pipeline(struct panvk_device *dev,
                            VkPipelineLayout *layout_out,
                            VkPipeline *pipeline_out)
{
   const enum panvk_meta_object_key_type layout_key =
      PANVK_META_OBJECT_KEY_BC_DECODE_AFRC_LAYOUT;
   const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(struct panvk_bc_afrc_push),
   };
   VkResult result =
      vk_meta_get_pipeline_layout(&dev->vk, &dev->meta, NULL, &push_range,
                                  &layout_key, sizeof(layout_key), layout_out);
   if (result != VK_SUCCESS)
      return result;

   const enum panvk_meta_object_key_type key =
      PANVK_META_OBJECT_KEY_BC_DECODE_AFRC_SHADER;
   VkPipeline cached = vk_meta_lookup_pipeline(&dev->meta, &key, sizeof(key));
   if (cached != VK_NULL_HANDLE) {
      *pipeline_out = cached;
      return VK_SUCCESS;
   }

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(panvk_bc_alpha_fs_spv),
      .pCode = panvk_bc_alpha_fs_spv,
   };
   const VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &module_info,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .pName = "main",
   };
   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 1,
      .pStages = &stage,
      .layout = *layout_out,
   };
   const struct vk_meta_rendering_info render = {
      .samples = 1,
      .color_attachment_count = 1,
      .color_attachment_formats = {VK_FORMAT_R8G8B8A8_UNORM},
      .color_attachment_write_masks = {VK_COLOR_COMPONENT_R_BIT |
                                       VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT |
                                       VK_COLOR_COMPONENT_A_BIT},
   };

   return vk_meta_create_graphics_pipeline(&dev->vk, &dev->meta, &info,
                                           &render, &key, sizeof(key),
                                           pipeline_out);
}

/* Decode one region into bc_shadow with a layered draw, so the tile
 * writeback produces the AFRC layout. Runs between meta_gfx_start() and
 * meta_gfx_end(). */
static void
bc_decode_region_afrc(struct panvk_cmd_buffer *cmdbuf, struct panvk_image *img,
                      const VkImageSubresourceLayers *subres, VkOffset3D offset,
                      VkExtent3D extent, const struct bc_decode_src *src)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const struct vk_device_dispatch_table *disp = &dev->vk.dispatch_table;
   VkCommandBuffer cmd = panvk_cmd_buffer_to_handle(cmdbuf);
   const uint32_t level = subres->mipLevel;
   const VkExtent3D mip = vk_image_mip_level_extent(&img->vk, level);
   const uint32_t x0 = offset.x & ~3, y0 = offset.y & ~3;
   const uint32_t w = MIN2(extent.width + (offset.x - x0), mip.width - x0);
   const uint32_t h = MIN2(extent.height + (offset.y - y0), mip.height - y0);
   const uint32_t layers = vk_image_subresource_layer_count(&img->vk, subres);

   if (!w || !h || !layers)
      return;

   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkResult result = get_bc_decode_afrc_pipeline(dev, &layout, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmdbuf->vk, result);
      return;
   }

   const VkImageViewUsageCreateInfo view_usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   };
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &view_usage,
      .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
      .image = img->bc_shadow,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = level,
         .levelCount = 1,
         .baseArrayLayer = subres->baseArrayLayer,
         .layerCount = layers,
      },
   };
   VkImageView view;
   result = vk_meta_create_image_view(&cmdbuf->vk, &dev->meta, &view_info,
                                      &view);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmdbuf->vk, result);
      return;
   }

   /* A whole-level decode rewrites every texel: skip the tile load. */
   const bool whole = x0 == 0 && y0 == 0 && w == mip.width && h == mip.height;
   const VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = whole ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                      : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };
   const VkRenderingInfo render = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.offset = {x0, y0}, .extent = {w, h}},
      .layerCount = layers,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };
   const struct panvk_bc_afrc_push push = {
      .src = {(uint32_t)src->addr, (uint32_t)(src->addr >> 32)},
      .src_row_B = src->row_B,
      .src_slice_B = src->slice_B,
      .x0 = x0,
      .y0 = y0,
      .bc2 = img->vk.format == VK_FORMAT_BC2_UNORM_BLOCK ||
             img->vk.format == VK_FORMAT_BC2_SRGB_BLOCK,
   };
   const struct vk_meta_rect rect = {
      .x0 = x0,
      .y0 = y0,
      .x1 = x0 + w,
      .y1 = y0 + h,
   };

   disp->CmdBeginRendering(cmd, &render);
   disp->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   disp->CmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof(push), &push);
   dev->meta.cmd_draw_volume(&cmdbuf->vk, &dev->meta, &rect, layers);
   disp->CmdEndRendering(cmd);
}

/* Decode source reading the raw plane of img itself. */
static struct bc_decode_src
bc_raw_plane_src(const struct panvk_image *img,
                 const VkImageSubresourceLayers *subres, VkOffset3D offset)
{
   const struct pan_image_layout *l = &img->planes[0].plane.layout;
   const struct pan_image_slice_layout *sl = &l->slices[subres->mipLevel];
   const uint32_t blk_B = vk_format_get_blocksize(img->vk.format);

   return (struct bc_decode_src){
      .addr = img->planes[0].plane.base + sl->offset_B +
              subres->baseArrayLayer * l->array_stride_B +
              (offset.y / 4) * sl->tiled_or_linear.row_stride_B +
              (offset.x / 4) * blk_B,
      .row_B = sl->tiled_or_linear.row_stride_B,
      .slice_B = l->array_stride_B,
   };
}

/* Max texels per BC decode dispatch along each axis, rounded down to whole
 * 4x4 blocks. 0 disables chunking (one dispatch per region, the behavior
 * before this option existed) and exists for A/B testing. Long single
 * dispatches can outlive a cross-CSG SYNC_WAIT of the other subqueues and
 * get the CSG killed by the firmware (CSF group fatal, status 0x72/0x41);
 * keeping every job short avoids that trigger. */
static uint32_t
bc_chunk_texels(void)
{
   const int64_t chunk = debug_get_num_option("PANVK_BC_CHUNK", 256);

   if (chunk <= 0)
      return 0; /* no chunking */

   return MAX2((uint32_t)chunk & ~3u, 4u);
}

static uint32_t bc_trace_count;

/* Transcode the BC blocks of one copy region into bc_decoded. They come
 * from src (block at the region origin, row and layer/slice strides) or,
 * when src is NULL, from the raw plane (planes[0]). */
static void
bc_decode_region(struct panvk_cmd_buffer *cmdbuf, struct panvk_image *img,
                 const VkImageSubresourceLayers *subres, VkOffset3D offset,
                 VkExtent3D extent, const struct bc_decode_src *src_in)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const struct vk_device_dispatch_table *disp = &dev->vk.dispatch_table;
   VkCommandBuffer cmd = panvk_cmd_buffer_to_handle(cmdbuf);
   VkPipelineLayout layout;
   VkPipeline pipeline;

   VkResult result = get_bc_decode_pipeline(
      dev, bc_decode_shader_index(img->vk.format), img->vk.format, &layout,
      &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmdbuf->vk, result);
      return;
   }

   const uint32_t level = subres->mipLevel;
   const VkExtent3D mip = vk_image_mip_level_extent(&img->vk, level);
   const bool is_3d = img->vk.image_type == VK_IMAGE_TYPE_3D;
   const struct pan_image_layout *sl = &img->planes[0].plane.layout;
   const struct pan_image_layout *dl = &img->bc_decoded.plane.layout;
   const struct pan_image_slice_layout *ss = &sl->slices[level];
   const struct pan_image_slice_layout *ds = &dl->slices[level];
   const unsigned blk_B = vk_format_get_blocksize(img->vk.format);

   const uint32_t x0 = offset.x & ~3, y0 = offset.y & ~3;
   const uint32_t z0 = is_3d ? offset.z : subres->baseArrayLayer;
   const uint32_t w = MIN2(extent.width + (offset.x - x0), mip.width - x0);
   const uint32_t h = MIN2(extent.height + (offset.y - y0), mip.height - y0);
   const uint32_t d = is_3d ? extent.depth
                            : vk_image_subresource_layer_count(&img->vk, subres);
   const uint64_t src_z_B =
      is_3d ? ss->tiled_or_linear.surface_stride_B : sl->array_stride_B;
   const uint64_t dst_z_B =
      is_3d ? ds->tiled_or_linear.surface_stride_B : dl->array_stride_B;

   if (!w || !h || !d)
      return;

   const uint64_t src = src_in ? src_in->addr
                               : img->planes[0].plane.base + ss->offset_B +
                                    z0 * src_z_B +
                                    (y0 / 4) * ss->tiled_or_linear.row_stride_B +
                                    (x0 / 4) * blk_B;

   /* ASTC planes are addressed in 4x4 blocks, R8/RG8 planes in texels. */
   const VkFormat dec_fmt = panvk_bc_decoded_format(img->vk.format);
   const bool dec_blocks = vk_format_is_compressed(dec_fmt);
   const unsigned dec_B = vk_format_get_blocksize(dec_fmt);
   const bool dst_tiled = img->bc_decoded.image.props.modifier ==
                          DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED;
   const uint32_t dst_x0 = dec_blocks ? x0 / 4 : x0;
   const uint32_t dst_y0 = dec_blocks ? y0 / 4 : y0;
   const uint64_t dst = img->bc_decoded.plane.base + ds->offset_B +
                        z0 * dst_z_B +
                        (dst_tiled ? 0 : dst_y0 * ds->tiled_or_linear.row_stride_B +
                                            dst_x0 * dec_B);

   uint64_t lut = 0;
   /* The EAC table also serves the ETC2 RGBA8 alpha of BC2/BC3. */
   const uint32_t shader_idx = bc_decode_shader_index(img->vk.format);
   if (shader_idx == BC_EAC_SHADER ||
       (shader_idx == BC_ETC_SHADER && img->vk.format >= VK_FORMAT_BC2_UNORM_BLOCK)) {
      lut = panvk_bc_eac_lut_addr(dev);
      if (!lut) {
         vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
   }

   const struct panvk_bc_decode_push push = {
      .src = {(uint32_t)src, (uint32_t)(src >> 32)},
      .dst = {(uint32_t)dst, (uint32_t)(dst >> 32)},
      .lut = {(uint32_t)lut, (uint32_t)(lut >> 32)},
      .src_row_B = src_in ? src_in->row_B : ss->tiled_or_linear.row_stride_B,
      .src_slice_B = src_in ? src_in->slice_B : src_z_B,
      .dst_row_B = ds->tiled_or_linear.row_stride_B,
      .dst_slice_B = dst_z_B,
      .width = w,
      .height = h,
      .depth = d,
      .format = img->vk.format,
      .dst_x0 = dst_x0,
      .dst_y0 = dst_y0,
      .dst_tiled = dst_tiled,
   };

   disp->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   const uint32_t chunk = bc_chunk_texels();
   const uint32_t chunk_w = chunk ? chunk : w;
   const uint32_t chunk_h = chunk ? chunk : h;

   const int64_t trace = debug_get_num_option("PANVK_BC_TRACE", 0);
   if (trace > 0 && p_atomic_inc_return(&bc_trace_count) <= (uint32_t)trace) {
      const uint32_t cw = MIN2(chunk_w, w);
      const uint32_t ch = MIN2(chunk_h, h);

      mesa_logi("bctrace: fmt=0x%x mip=%u extent=%ux%ux%u chunk=%u grid=%ux%u",
                img->vk.format, level, w, h, d, chunk,
                DIV_ROUND_UP(DIV_ROUND_UP(cw, 4), 8),
                DIV_ROUND_UP(DIV_ROUND_UP(ch, 4), 8));
   }

   for (uint32_t cy = 0; cy < h; cy += chunk_h) {
      const uint32_t ch = MIN2(chunk_h, h - cy);
      const uint32_t cblk_y = cy / 4;
      const uint64_t csrc_y = src + (uint64_t)cblk_y * push.src_row_B;

      for (uint32_t cx = 0; cx < w; cx += chunk_w) {
         const uint32_t cw = MIN2(chunk_w, w - cx);
         const uint32_t cblk_x = cx / 4;
         struct panvk_bc_decode_push p = push;

         const uint64_t csrc = csrc_y + (uint64_t)cblk_x * blk_B;

         p.src[0] = (uint32_t)csrc;
         p.src[1] = (uint32_t)(csrc >> 32);
         p.width = cw;
         p.height = ch;

         if (dst_tiled) {
            /* Tiled targets are addressed in absolute element coordinates. */
            p.dst_x0 = dst_x0 + (dec_blocks ? cblk_x : cx);
            p.dst_y0 = dst_y0 + (dec_blocks ? cblk_y : cy);
         } else {
            /* dst_row_B is a block row for compressed targets and a texel row
             * for the R8/RG8 fallback. */
            const uint64_t cdst =
               dst + (uint64_t)(dec_blocks ? cblk_y : cy) * push.dst_row_B +
               (uint64_t)(dec_blocks ? cblk_x * dec_B : cx * dec_B);

            p.dst[0] = (uint32_t)cdst;
            p.dst[1] = (uint32_t)(cdst >> 32);
         }

         disp->CmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                sizeof(p), &p);
         disp->CmdDispatch(cmd, DIV_ROUND_UP(DIV_ROUND_UP(cw, 4), 8),
                           DIV_ROUND_UP(DIV_ROUND_UP(ch, 4), 8), d);
      }
   }
}

static void
bc_decode_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dst_stage,
                  VkAccessFlags2 dst_access)
{
   const VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
   };
   const VkDependencyInfo dep_info = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mem_barrier,
   };
   panvk_per_arch(CmdPipelineBarrier2)(cmd, &dep_info);
}

static void
bc_decode_begin(struct panvk_cmd_buffer *cmdbuf,
                struct panvk_cmd_meta_compute_save_ctx *save)
{
   bc_decode_barrier(panvk_cmd_buffer_to_handle(cmdbuf),
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
   meta_compute_start(cmdbuf, save);
}

void
panvk_per_arch(cmd_bc_decode_zero_initialized)(
   struct panvk_cmd_buffer *cmdbuf, const VkDependencyInfo *dep_info)
{
   struct panvk_cmd_meta_compute_save_ctx save = {0};
   bool started = false, any_afrc = false;

   for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *b = &dep_info->pImageMemoryBarriers[i];
      VK_FROM_HANDLE(panvk_image, img, b->image);

      if (b->oldLayout != VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT ||
          b->newLayout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT ||
          !img->bc_emul)
         continue;

      if (img->bc_afrc) {
         any_afrc = true;
         continue;
      }

      if (!started) {
         bc_decode_begin(cmdbuf, &save);
         started = true;
      }

      const uint32_t level_count =
         vk_image_subresource_level_count(&img->vk, &b->subresourceRange);
      const uint32_t layer_count =
         vk_image_subresource_layer_count(&img->vk, &b->subresourceRange);
      for (uint32_t l = 0; l < level_count; l++) {
         const uint32_t level = b->subresourceRange.baseMipLevel + l;
         const VkImageSubresourceLayers subres = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = level,
            .baseArrayLayer = b->subresourceRange.baseArrayLayer,
            .layerCount = layer_count,
         };
         struct bc_decode_src zero_src = {0};
         if (img->bc_noraw) {
            /* Every block decodes the same all-zero BC block. */
            static const uint32_t zero_block[4] = {0};
            zero_src.addr = panvk_cmd_upload_dev_mem(cmdbuf, desc, zero_block,
                                                     sizeof(zero_block), 16)
                               .gpu;
         }
         bc_decode_region(cmdbuf, img, &subres, (VkOffset3D){0, 0, 0},
                          vk_image_mip_level_extent(&img->vk, level),
                          img->bc_noraw ? &zero_src : NULL);
      }
   }

   if (started)
      meta_compute_end(cmdbuf, &save);

   if (any_afrc) {
      struct panvk_cmd_meta_graphics_save_ctx gsave = {0};

      if (!started)
         bc_decode_barrier(panvk_cmd_buffer_to_handle(cmdbuf),
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
      meta_gfx_start(cmdbuf, &gsave);
      for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
         const VkImageMemoryBarrier2 *b = &dep_info->pImageMemoryBarriers[i];
         VK_FROM_HANDLE(panvk_image, img, b->image);

         if (b->oldLayout != VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT ||
             b->newLayout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT ||
             !img->bc_afrc)
            continue;

         const uint32_t level_count =
            vk_image_subresource_level_count(&img->vk, &b->subresourceRange);
         for (uint32_t l = 0; l < level_count; l++) {
            const VkImageSubresourceLayers subres = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .mipLevel = b->subresourceRange.baseMipLevel + l,
               .baseArrayLayer = b->subresourceRange.baseArrayLayer,
               .layerCount = vk_image_subresource_layer_count(
                  &img->vk, &b->subresourceRange),
            };
            const struct bc_decode_src src =
               bc_raw_plane_src(img, &subres, (VkOffset3D){0, 0, 0});
            bc_decode_region_afrc(
               cmdbuf, img, &subres, (VkOffset3D){0, 0, 0},
               vk_image_mip_level_extent(&img->vk, subres.mipLevel), &src);
         }
      }
      meta_gfx_end(cmdbuf, &gsave);
   }

   if (started || any_afrc)
      bc_decode_barrier(panvk_cmd_buffer_to_handle(cmdbuf),
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyMemoryToImageKHR)(
   VkCommandBuffer commandBuffer,
   const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo)
{
   VK_FROM_HANDLE(panvk_image, bc_img, pCopyMemoryInfo->image);

   if (bc_img->bc_afrc) {
      VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
      struct panvk_cmd_meta_graphics_save_ctx save = {0};
      const uint32_t blk_B = vk_format_get_blocksize(bc_img->vk.format);

      copy_memory_to_image_raw(commandBuffer, pCopyMemoryInfo);

      /* Decode straight from the copy source: the raw copy and the decode
       * only share reads, so they need no barrier between them; the app's
       * copy-stage barriers cover both. */
      meta_gfx_start(cmdbuf, &save);
      for (uint32_t i = 0; i < pCopyMemoryInfo->regionCount; i++) {
         const VkDeviceMemoryImageCopyKHR *r = &pCopyMemoryInfo->pRegions[i];
         const uint32_t row_px =
            r->addressRowLength ? r->addressRowLength : r->imageExtent.width;
         const uint32_t col_px = r->addressImageHeight ? r->addressImageHeight
                                                       : r->imageExtent.height;
         const uint32_t row_B = DIV_ROUND_UP(row_px, 4) * blk_B;
         const struct bc_decode_src src = {
            .addr = r->addressRange.address,
            .row_B = row_B,
            .slice_B = DIV_ROUND_UP(col_px, 4) * row_B,
         };

         bc_decode_region_afrc(cmdbuf, bc_img, &r->imageSubresource,
                               r->imageOffset, r->imageExtent, &src);
      }
      meta_gfx_end(cmdbuf, &save);
      return;
   }

   if (bc_img->bc_emul) {
      VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
      struct panvk_cmd_meta_compute_save_ctx save = {0};
      const uint32_t blk_B = vk_format_get_blocksize(bc_img->vk.format);

      /* Without a raw plane, decode straight from the copy source. */
      if (!bc_img->bc_noraw)
         copy_memory_to_image_raw(commandBuffer, pCopyMemoryInfo);

      bc_decode_begin(cmdbuf, &save);
      for (uint32_t i = 0; i < pCopyMemoryInfo->regionCount; i++) {
         const VkDeviceMemoryImageCopyKHR *r = &pCopyMemoryInfo->pRegions[i];
         const uint32_t row_px =
            r->addressRowLength ? r->addressRowLength : r->imageExtent.width;
         const uint32_t col_px = r->addressImageHeight ? r->addressImageHeight
                                                       : r->imageExtent.height;
         const uint32_t row_B = DIV_ROUND_UP(row_px, 4) * blk_B;
         const struct bc_decode_src src = {
            .addr = r->addressRange.address,
            .row_B = row_B,
            .slice_B = DIV_ROUND_UP(col_px, 4) * row_B,
         };
         bc_decode_region(cmdbuf, bc_img, &r->imageSubresource, r->imageOffset,
                          r->imageExtent, bc_img->bc_noraw ? &src : NULL);
      }
      meta_compute_end(cmdbuf, &save);
      return;
   }

   copy_memory_to_image_raw(commandBuffer, pCopyMemoryInfo);
}

static void
copy_memory_to_image_raw(
   VkCommandBuffer commandBuffer,
   const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   VK_FROM_HANDLE(panvk_image, img, pCopyMemoryInfo->image);

   /* Early out if this operation was lowered. */
   if (lower_copy_memory_to_image(commandBuffer, pCopyMemoryInfo))
      return;

   const bool use_gfx_pipeline = copy_to_image_use_gfx_pipeline(img);
   struct vk_meta_copy_image_properties img_props =
      panvk_meta_copy_get_image_properties(img, use_gfx_pipeline, true);

   if (use_gfx_pipeline) {
      struct panvk_cmd_meta_graphics_save_ctx save = {0};

      meta_gfx_start(cmdbuf, &save);
      vk_meta_copy_memory_to_image(&cmdbuf->vk, &dev->meta,
                                   pCopyMemoryInfo, &img_props,
                                   VK_PIPELINE_BIND_POINT_GRAPHICS);
      meta_gfx_end(cmdbuf, &save);
   } else {
      struct panvk_cmd_meta_compute_save_ctx save = {0};

      meta_compute_start(cmdbuf, &save);
#if PAN_ARCH >= 10
      if (copy_buffer_to_image_touches_crc(img, pCopyMemoryInfo)) {
         struct cs_builder *b =
            panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);

         panvk_per_arch(cmd_invalidate_crc)(
            b, panvk_image_plane_crc_header_addr(img));
      }
#endif
      vk_meta_copy_memory_to_image(&cmdbuf->vk, &dev->meta,
                                   pCopyMemoryInfo, &img_props,
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
      meta_compute_end(cmdbuf, &save);
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyImageToMemoryKHR)(
   VkCommandBuffer commandBuffer,
   const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   VK_FROM_HANDLE(panvk_image, img, pCopyMemoryInfo->image);
   struct vk_meta_copy_image_properties img_props =
      panvk_meta_copy_get_image_properties(img, false, false);
   struct panvk_cmd_meta_compute_save_ctx save = {0};

   if (img->bc_noraw) {
      /* The raw BC blocks were never stored; PANVK_BC_KEEP_RAW=1 keeps
       * them for applications that read compressed images back. */
      static bool warned;
      if (!warned) {
         mesa_logw("panvk: BC image readback without raw blocks is skipped "
                   "(set PANVK_BC_KEEP_RAW=1)");
         warned = true;
      }
      return;
   }

   meta_compute_start(cmdbuf, &save);
   vk_meta_copy_image_to_memory(&cmdbuf->vk, &dev->meta, pCopyMemoryInfo,
                                &img_props);
   meta_compute_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdFillBuffer)(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                              VkDeviceSize dstOffset, VkDeviceSize fillSize,
                              uint32_t data)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   VK_FROM_HANDLE(panvk_buffer, buffer, dstBuffer);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(cmdbuf->vk.base.device->physical);

   uint64_t addr = panvk_buffer_gpu_ptr(buffer, dstOffset);
   uint64_t range = panvk_buffer_range(buffer, dstOffset, fillSize) & ~3ULL;
   if (!range)
      return;

   const uint32_t max_wg = phys_dev->vk.properties.maxComputeWorkGroupCount[0];
   struct panvk_precomp_ctx ctx = panvk_per_arch(precomp_cs)(cmdbuf);

   const bool uint4_path =
      util_is_aligned(addr, 16) && util_is_aligned(range, 16);
   const uint32_t elem_size = uint4_path ? 16 : 4;
   const uint32_t wg_bytes = 32 * elem_size;

   while (range >= wg_bytes) {
      const uint32_t wgs = MIN2(range / wg_bytes, max_wg);
      const uint64_t bulk = (uint64_t)wgs * wg_bytes;

      if (uint4_path) {
         panlib_fill_uint4(&ctx, panlib_1d(wgs), PANLIB_BARRIER_NONE, addr,
                           data, data, data, data);
      } else {
         panlib_fill(&ctx, panlib_1d(wgs), PANLIB_BARRIER_NONE, addr, data);
      }

      addr += bulk;
      range -= bulk;
   }

   if (range) {
      const uint32_t tail = range / elem_size;

      if (uint4_path) {
         panlib_fill_uint4_scalar(&ctx, panlib_1d(tail), PANLIB_BARRIER_NONE,
                                  addr, data, data, data, data);
      } else {
         panlib_fill_scalar(&ctx, panlib_1d(tail), PANLIB_BARRIER_NONE, addr,
                            data);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdUpdateBuffer)(VkCommandBuffer commandBuffer,
                                VkBuffer dstBuffer, VkDeviceSize dstOffset,
                                VkDeviceSize dataSize, const void *pData)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_compute_save_ctx save = {0};

   meta_compute_start(cmdbuf, &save);
   vk_meta_update_buffer(&cmdbuf->vk, &dev->meta, dstBuffer, dstOffset,
                         dataSize, pData);
   meta_compute_end(cmdbuf, &save);
}

static bool
lower_copy_image(VkCommandBuffer commandBuffer,
                 const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(panvk_image, dst_img, pCopyImageInfo->dstImage);

   const VkImageAspectFlags zs_mask =
      (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   /* Only required for interleaved depth stencil that are not multi-planar */
   if (vk_format_aspects(dst_img->vk.format) != zs_mask ||
       dst_img->plane_count > 1)
      return false;

   uint32_t num_depth_regions = 0, num_stencil_regions = 0;
   for (uint32_t i = 0; i < pCopyImageInfo->regionCount; i++) {
      const VkImageAspectFlags aspect_mask =
         pCopyImageInfo->pRegions[i].dstSubresource.aspectMask;
      assert((aspect_mask & ~zs_mask) == 0);
      if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
         num_depth_regions++;
      else
         num_stencil_regions++;
   }

   /* If we have both depth and stencil writes to an interleaved depth stencil
    * image, we must split the writes per aspect with a barrier between them to
    * avoid a write-after-write race. */
   const bool lowering_needed = (num_depth_regions && num_stencil_regions);
   if (!lowering_needed)
      return false;

   VkCopyImageInfo2 adjusted_info = *pCopyImageInfo;
   STACK_ARRAY(VkImageCopy2, depth_regions, num_depth_regions);
   STACK_ARRAY(VkImageCopy2, stencil_regions, num_stencil_regions);

   uint32_t depth_idx = 0, stencil_idx = 0;
   for (uint32_t i = 0; i < pCopyImageInfo->regionCount; i++) {
      const VkImageAspectFlags aspect_mask =
         pCopyImageInfo->pRegions[i].dstSubresource.aspectMask;

      if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
         depth_regions[depth_idx++] = pCopyImageInfo->pRegions[i];
      else
         stencil_regions[stencil_idx++] = pCopyImageInfo->pRegions[i];
   }

   adjusted_info.regionCount = num_depth_regions;
   adjusted_info.pRegions = depth_regions;
   panvk_per_arch(CmdCopyImage2)(commandBuffer, &adjusted_info);

   const VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
   const VkDependencyInfo dep_info = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mem_barrier,
   };
   panvk_per_arch(CmdPipelineBarrier2)(commandBuffer, &dep_info);

   adjusted_info.regionCount = num_stencil_regions;
   adjusted_info.pRegions = stencil_regions;
   panvk_per_arch(CmdCopyImage2)(commandBuffer, &adjusted_info);

   STACK_ARRAY_FINISH(depth_regions);
   STACK_ARRAY_FINISH(stencil_regions);

   return true;
}

/* Both decoded planes hold the same element type in the same tiling, so a
 * BC copy can move decoded elements instead of re-decoding raw blocks. */
static bool
bc_can_copy_decoded(const struct panvk_image *src, const struct panvk_image *dst)
{
   return src->bc_emul && dst->bc_emul && !src->bc_afrc && !dst->bc_afrc &&
          panvk_bc_decoded_format(src->vk.format) ==
             panvk_bc_decoded_format(dst->vk.format) &&
          src->bc_decoded.image.props.modifier ==
             DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED &&
          dst->bc_decoded.image.props.modifier ==
             DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED;
}

/* Base of (level, first layer or slice) in a decoded plane and the stride
 * to the next layer/slice. */
static uint64_t
bc_decoded_base(const struct panvk_image *img,
                const VkImageSubresourceLayers *subres, int32_t z,
                uint32_t *z_stride_B)
{
   const struct pan_image_layout *l = &img->bc_decoded.plane.layout;
   const struct pan_image_slice_layout *sl = &l->slices[subres->mipLevel];
   const bool is_3d = img->vk.image_type == VK_IMAGE_TYPE_3D;

   *z_stride_B = is_3d ? sl->tiled_or_linear.surface_stride_B : l->array_stride_B;
   return img->bc_decoded.plane.base + sl->offset_B +
          (uint64_t)(is_3d ? z : subres->baseArrayLayer) * *z_stride_B;
}

static void
bc_copy_decoded_region(struct panvk_cmd_buffer *cmdbuf,
                       const struct panvk_image *src,
                       const struct panvk_image *dst, const VkImageCopy2 *r)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const struct vk_device_dispatch_table *disp = &dev->vk.dispatch_table;
   VkCommandBuffer cmd = panvk_cmd_buffer_to_handle(cmdbuf);
   VkPipelineLayout layout;
   VkPipeline pipeline;

   if (get_bc_decode_pipeline(dev, BC_COPY_SHADER, VK_FORMAT_UNDEFINED, &layout,
                              &pipeline) != VK_SUCCESS)
      return;

   const VkFormat dec = panvk_bc_decoded_format(dst->vk.format);
   const bool astc = vk_format_is_compressed(dec);
   /* ASTC/EAC: 4x4 blocks in tiles of 4x4 blocks. R8/RG8: 2x2 quads in
    * tiles of 8x8 quads. Copy regions of BC images are 4x4 aligned. */
   const uint32_t el_px = astc ? 4 : 2;
   const uint32_t elem_B = vk_format_get_blocksize(dec) * (astc ? 1 : 4);
   const bool is_3d = dst->vk.image_type == VK_IMAGE_TYPE_3D;
   const uint32_t depth = is_3d ? r->extent.depth
                                : vk_image_subresource_layer_count(&dst->vk,
                                                                   &r->dstSubresource);
   uint32_t src_z_B, dst_z_B;
   const uint64_t sbase =
      bc_decoded_base(src, &r->srcSubresource, r->srcOffset.z, &src_z_B);
   const uint64_t dbase =
      bc_decoded_base(dst, &r->dstSubresource, r->dstOffset.z, &dst_z_B);
   const uint32_t w = DIV_ROUND_UP(ALIGN_POT(r->extent.width, 4), el_px);
   const uint32_t h = DIV_ROUND_UP(ALIGN_POT(r->extent.height, 4), el_px);

   const struct panvk_bc_decode_push push = {
      .src = {(uint32_t)sbase, (uint32_t)(sbase >> 32)},
      .dst = {(uint32_t)dbase, (uint32_t)(dbase >> 32)},
      .src_row_B = src->bc_decoded.plane.layout.slices[r->srcSubresource.mipLevel]
                      .tiled_or_linear.row_stride_B,
      .src_slice_B = src_z_B,
      .dst_row_B = dst->bc_decoded.plane.layout.slices[r->dstSubresource.mipLevel]
                      .tiled_or_linear.row_stride_B,
      .dst_slice_B = dst_z_B,
      .width = w,
      .height = h,
      .depth = depth,
      .format = elem_B,
      .dst_x0 = r->dstOffset.x / el_px,
      .dst_y0 = r->dstOffset.y / el_px,
      .dst_tiled = astc ? 2 : 3,
      .src_x0 = r->srcOffset.x / el_px,
      .src_y0 = r->srcOffset.y / el_px,
   };

   disp->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   disp->CmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(push), &push);
   disp->CmdDispatch(cmd, DIV_ROUND_UP(w, 8), DIV_ROUND_UP(h, 8), depth);
}

/* Copies touching an image without raw BC blocks: move decoded elements
 * when both sides decode alike, otherwise decode into the destination from
 * the source's raw blocks. */
static void
bc_copy_image_noraw(struct panvk_cmd_buffer *cmdbuf,
                    const VkCopyImageInfo2 *info, struct panvk_image *src,
                    struct panvk_image *dst)
{
   const bool decoded = bc_can_copy_decoded(src, dst);
   const bool from_raw = dst->bc_noraw && src->bc_emul && !src->bc_noraw &&
                         panvk_format_is_bc(src->vk.format);

   if (!decoded && !from_raw) {
      static bool warned;
      if (!warned) {
         mesa_logw("panvk: unsupported copy with a BC image without raw "
                   "blocks (format %d -> %d), skipped; set PANVK_BC_KEEP_RAW=1",
                   src->vk.format, dst->vk.format);
         warned = true;
      }
      return;
   }

   if (decoded && !dst->bc_noraw) {
      /* Keep the destination's raw blocks consistent where they exist. */
      static bool warned_raw;
      if (!warned_raw) {
         mesa_logw("panvk: BC copy leaves destination raw blocks stale");
         warned_raw = true;
      }
   }

   struct panvk_cmd_meta_compute_save_ctx save = {0};

   bc_decode_begin(cmdbuf, &save);
   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkImageCopy2 *r = &info->pRegions[i];

      if (decoded) {
         bc_copy_decoded_region(cmdbuf, src, dst, r);
      } else {
         const struct bc_decode_src s =
            bc_raw_plane_src(src, &r->srcSubresource, r->srcOffset);
         bc_decode_region(cmdbuf, dst, &r->dstSubresource, r->dstOffset,
                          r->extent, &s);
      }
   }
   meta_compute_end(cmdbuf, &save);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyImage2)(VkCommandBuffer commandBuffer,
                              const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(panvk_image, bc_src, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(panvk_image, bc_dst, pCopyImageInfo->dstImage);

   if (bc_src->bc_noraw || bc_dst->bc_noraw) {
      VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
      bc_copy_image_noraw(cmdbuf, pCopyImageInfo, bc_src, bc_dst);
      return;
   }

   copy_image_raw(commandBuffer, pCopyImageInfo);

   if (!bc_dst->bc_emul)
      return;

   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   const bool src_blocks = vk_format_is_compressed(bc_src->vk.format);

   if (bc_dst->bc_afrc) {
      struct panvk_cmd_meta_graphics_save_ctx gsave = {0};

      /* Decode from the raw blocks the copy just wrote. */
      bc_decode_barrier(commandBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
      meta_gfx_start(cmdbuf, &gsave);
      for (uint32_t i = 0; i < pCopyImageInfo->regionCount; i++) {
         const VkImageCopy2 *r = &pCopyImageInfo->pRegions[i];
         VkExtent3D extent = r->extent;

         if (!src_blocks) {
            extent.width *= 4;
            extent.height *= 4;
         }
         const struct bc_decode_src src =
            bc_raw_plane_src(bc_dst, &r->dstSubresource, r->dstOffset);
         bc_decode_region_afrc(cmdbuf, bc_dst, &r->dstSubresource,
                               r->dstOffset, extent, &src);
      }
      meta_gfx_end(cmdbuf, &gsave);
      return;
   }

   struct panvk_cmd_meta_compute_save_ctx save = {0};

   bc_decode_begin(cmdbuf, &save);
   for (uint32_t i = 0; i < pCopyImageInfo->regionCount; i++) {
      const VkImageCopy2 *r = &pCopyImageInfo->pRegions[i];
      VkExtent3D extent = r->extent;

      if (!src_blocks) {
         extent.width *= 4;
         extent.height *= 4;
      }
      bc_decode_region(cmdbuf, bc_dst, &r->dstSubresource, r->dstOffset,
                       extent, NULL);
   }
   meta_compute_end(cmdbuf, &save);
}

static void
copy_image_raw(VkCommandBuffer commandBuffer,
               const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   VK_FROM_HANDLE(panvk_image, src_img, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(panvk_image, dst_img, pCopyImageInfo->dstImage);

   /* Early out if this operation was lowered. */
   if (lower_copy_image(commandBuffer, pCopyImageInfo))
      return;

   const bool use_gfx_pipeline = copy_to_image_use_gfx_pipeline(dst_img);
   struct vk_meta_copy_image_properties dst_img_props =
      panvk_meta_copy_get_image_properties(dst_img, use_gfx_pipeline, true);
   struct vk_meta_copy_image_properties src_img_props =
      panvk_meta_copy_get_image_properties(src_img, use_gfx_pipeline, false);

   if (use_gfx_pipeline) {
      struct panvk_cmd_meta_graphics_save_ctx save = {0};

      meta_gfx_start(cmdbuf, &save);
      vk_meta_copy_image(&cmdbuf->vk, &dev->meta, pCopyImageInfo,
                         &src_img_props, &dst_img_props,
                         VK_PIPELINE_BIND_POINT_GRAPHICS);
      meta_gfx_end(cmdbuf, &save);
   } else {
      struct panvk_cmd_meta_compute_save_ctx save = {0};

      meta_compute_start(cmdbuf, &save);
#if PAN_ARCH >= 10
      if (copy_image_touches_crc(dst_img, pCopyImageInfo)) {
         struct cs_builder *b =
            panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);

         panvk_per_arch(cmd_invalidate_crc)(
            b, panvk_image_plane_crc_header_addr(dst_img));
      }
#endif
      vk_meta_copy_image(&cmdbuf->vk, &dev->meta, pCopyImageInfo,
                         &src_img_props, &dst_img_props,
                         VK_PIPELINE_BIND_POINT_COMPUTE);
      meta_compute_end(cmdbuf, &save);
   }
}

void
panvk_per_arch(cmd_meta_resolve_attachments)(struct panvk_cmd_buffer *cmdbuf)
{
   struct pan_fb_layout *fb = &cmdbuf->state.gfx.render.fb.layout;
   bool needs_resolve = false;

   unsigned bound_atts = cmdbuf->state.gfx.render.bound_attachments;
   unsigned color_att_count =
      util_last_bit(bound_atts & MESA_VK_RP_ATTACHMENT_ANY_COLOR_BITS);
   VkRenderingAttachmentInfo color_atts[MAX_RTS];
   for (uint32_t i = 0; i < color_att_count; i++) {

      const struct panvk_resolve_attachment *resolve_info =
         &cmdbuf->state.gfx.render.color_attachments.resolve[i];
      struct panvk_image_view *src_iview =
         cmdbuf->state.gfx.render.color_attachments.iviews[i];

      color_atts[i] = (VkRenderingAttachmentInfo){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = panvk_image_view_to_handle(src_iview),
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .resolveMode = resolve_info->mode,
         .resolveImageView =
            panvk_image_view_to_handle(resolve_info->dst_iview),
         .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };

      if (resolve_info->mode != VK_RESOLVE_MODE_NONE)
         needs_resolve = true;

      if (resolve_info->mode != VK_RESOLVE_MODE_NONE) {
         assert(src_iview->pview.nr_samples > 1);
         assert(resolve_info->dst_iview->pview.nr_samples == 1);
      }
   }

   const struct panvk_resolve_attachment *resolve_info =
      &cmdbuf->state.gfx.render.z_attachment.resolve;
   struct panvk_image_view *src_iview =
      cmdbuf->state.gfx.render.z_attachment.iview;
   VkRenderingAttachmentInfo z_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = panvk_image_view_to_handle(src_iview),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = resolve_info->mode,
      .resolveImageView = panvk_image_view_to_handle(resolve_info->dst_iview),
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };

   if (resolve_info->mode != VK_RESOLVE_MODE_NONE)
      needs_resolve = true;

   resolve_info = &cmdbuf->state.gfx.render.s_attachment.resolve;
   src_iview = cmdbuf->state.gfx.render.s_attachment.iview;

   VkRenderingAttachmentInfo s_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = panvk_image_view_to_handle(src_iview),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = resolve_info->mode,
      .resolveImageView = panvk_image_view_to_handle(resolve_info->dst_iview),
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };

   if (resolve_info->mode != VK_RESOLVE_MODE_NONE)
      needs_resolve = true;

   if (!needs_resolve)
      return;

#if PAN_ARCH >= 10
   /* insert a barrier for resolve */
   const VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
   };
   const VkDependencyInfo dep_info = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mem_barrier,
   };
   panvk_per_arch(CmdPipelineBarrier2)(panvk_cmd_buffer_to_handle(cmdbuf),
                                       &dep_info);
#endif

   const VkRenderingInfo render_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea =
         {
            .offset.x = fb->render_area_px.min_x,
            .offset.y = fb->render_area_px.min_y,
            .extent.width =
               fb->render_area_px.max_x - fb->render_area_px.min_x + 1,
            .extent.height =
               fb->render_area_px.max_y - fb->render_area_px.min_y + 1,
         },
      .layerCount = cmdbuf->state.gfx.render.layer_count,
      .viewMask = cmdbuf->state.gfx.render.view_mask,
      .colorAttachmentCount = color_att_count,
      .pColorAttachments = color_atts,
      .pDepthAttachment = &z_att,
      .pStencilAttachment = &s_att,
   };

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_cmd_meta_graphics_save_ctx save = {0};

   meta_gfx_start(cmdbuf, &save);
   vk_meta_resolve_rendering(&cmdbuf->vk, &dev->meta, &render_info);
   meta_gfx_end(cmdbuf, &save);
}

#if PAN_ARCH >= 10

#define COPY_MEM_INDIRECT_MAX_WG 16
#define COPY_MEM_INDIRECT_WG_BYTES                                            \
   (PANLIB_COPY_MEM_WG_SIZE * PANLIB_COPY_MEM_CHUNK_SIZE)

/* Turn the 64-bit byte size at size_addr into a workgroup count in
 * JOB_SIZE_X, capped at COPY_MEM_INDIRECT_MAX_WG. Pre-v13 archs have no CS
 * shift instructions, so the count is only approximated with
 * min(size, cap). The result is never too small, the kernel loops when the
 * dispatch does not cover the whole size.
 */
static void
emit_copy_mem_indirect_wg_count(struct panvk_cmd_buffer *cmdbuf,
                                uint64_t size_addr)
{
   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);

   cs_update_compute_ctx(b) {
      cs_move64_to(b, cs_scratch_reg64(b, 0), size_addr);
      cs_load_to(b, cs_scratch_reg_tuple(b, 2, 2), cs_scratch_reg64(b, 0),
                 BITFIELD_MASK(2), 0);
      cs_flush_loads(b);
#if PAN_ARCH >= 13
      /* wg_count = DIV_ROUND_UP(size, COPY_MEM_INDIRECT_WG_BYTES) */
      cs_add_imm64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 2),
                   COPY_MEM_INDIRECT_WG_BYTES - 1);
      cs_rshift_imm_u64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 2),
                        util_logbase2(COPY_MEM_INDIRECT_WG_BYTES));
#endif
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_X),
                   COPY_MEM_INDIRECT_MAX_WG);
      cs_umin32(b, cs_scratch_reg32(b, 2), cs_scratch_reg32(b, 2),
                cs_sr_reg32(b, COMPUTE, JOB_SIZE_X));
      /* Keep the cap if the size exceeds 32 bits. */
      cs_if(b, MALI_CS_CONDITION_EQUAL, cs_scratch_reg32(b, 3)) {
         cs_move_reg32(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_X),
                       cs_scratch_reg32(b, 2));
      }
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_Y), 1);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_Z), 1);
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyMemoryIndirectKHR)(
   VkCommandBuffer commandBuffer,
   const VkCopyMemoryIndirectInfoKHR *pCopyMemoryIndirectInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
#if PAN_ARCH >= 10
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_META);
#endif
   struct panvk_precomp_ctx ctx = panvk_per_arch(precomp_cs)(cmdbuf);

   for (uint32_t i = 0; i < pCopyMemoryIndirectInfo->copyCount; i++) {
      uint64_t cmd_addr = pCopyMemoryIndirectInfo->copyAddressRange.address +
                          i * pCopyMemoryIndirectInfo->copyAddressRange.stride;

      emit_copy_mem_indirect_wg_count(
         cmdbuf, cmd_addr + offsetof(VkCopyMemoryIndirectCommandKHR, size));
      panlib_copy_mem_indirect(&ctx, panlib_dynamic_csf(),
                               PANLIB_BARRIER_CSF_SYNC, cmd_addr);
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyMemoryToImageIndirectKHR)(
   VkCommandBuffer commandBuffer,
   const VkCopyMemoryToImageIndirectInfoKHR *pCopyMemoryToImageIndirectInfo)
{
   assert(!"indirectMemoryToImageCopy is not supported");
}

#endif /* PAN_ARCH >= 10 */
