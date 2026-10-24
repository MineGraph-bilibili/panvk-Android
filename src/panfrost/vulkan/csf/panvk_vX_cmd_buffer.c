/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2026 Arm Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "drm-uapi/panthor_drm.h"

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_alloc.h"
#include <inttypes.h>
#include "util/u_atomic.h"
#include "util/os_time.h"
#include <unistd.h>
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_cmd_pool.h"
#include "panvk_cmd_push_constant.h"
#include "panvk_cmd_ts.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_instance.h"
#include "panvk_instr.h"
#include "panvk_meta.h"
#include "panvk_physical_device.h"
#include "panvk_priv_bo.h"
#include "panvk_tracepoints.h"
#include "panvk_utrace.h"

#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_props.h"
#include "pan_samples.h"

#include "util/bitscan.h"
#include "vk_descriptor_update_template.h"
#include "vk_format.h"
#include "vk_synchronization.h"

void
panvk_per_arch(kbase_mark_progress)(
   struct panvk_cmd_buffer *cmdbuf, enum panvk_subqueue_id subqueue,
   enum panvk_kbase_progress_marker marker)
{
   /* These stores are breadcrumbs for queue-hang diagnosis, not part of the
    * queue ABI.  Keeping them in every draw/dispatch stream adds several LS
    * instructions and a store flush at each marker, which is particularly
    * costly for draw-heavy applications.  Keep the instrumentation opt-in. */
   if (!PANVK_DEBUG(KBASE_DIAG))
      return;

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   if (!phys_dev->kbase_node_path[0])
      return;

   enum {
      KBASE_MARK_ADDR_REG = 14,
      KBASE_MARK_VALUE_REG = 16,
   };
   STATIC_ASSERT(KBASE_MARK_ADDR_REG + 2 <= CS_REG_SCRATCH_COUNT);
   STATIC_ASSERT(KBASE_MARK_VALUE_REG + 1 <= CS_REG_SCRATCH_COUNT);

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, subqueue);
   struct cs_index addr = cs_scratch_reg64(b, KBASE_MARK_ADDR_REG);
   struct cs_index value = cs_scratch_reg32(b, KBASE_MARK_VALUE_REG);

   cs_load64_to(b, addr, cs_subqueue_ctx_reg(b),
                offsetof(struct panvk_cs_subqueue_context,
                         debug.kbase_progress_addr));
   cs_move32_to(b, value, marker);
   // cs_store32(b, value, addr, 0);
   cs_sync32_add(b, true, MALI_CS_SYNC_SCOPE_CSG, value, addr, cs_now());
   cs_flush_stores(b);
}

/* Mali indexes TLS by (core, thread slot), so concurrent jobs from any
 * command buffer can share one region sized for the largest per-thread
 * stack, exactly like the subqueues of one command buffer already do.
 * Per-command-buffer TLS kept ~84 MiB per DXVK command list alive in Need
 * for Speed Most Wanted (1.7 GiB, then Android killed apps).
 * ponytail: outgrown BOs live until device destroy (<= 2x the peak size);
 * refcount them per command buffer if that ever matters. */
static uint64_t
get_device_tls(struct panvk_cmd_buffer *cmdbuf, uint64_t size)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   uint64_t addr = 0;

   simple_mtx_lock(&dev->tls.lock);
   struct panvk_priv_bo *cur =
      util_dynarray_num_elements(&dev->tls.bos, struct panvk_priv_bo *)
         ? util_dynarray_top(&dev->tls.bos, struct panvk_priv_bo *)
         : NULL;

   if (!cur || pan_kmod_bo_size(cur->bo) < size) {
      struct panvk_priv_bo *bo;
      if (panvk_priv_bo_create(
             dev, size,
             panvk_device_adjust_bo_flags(dev, PAN_KMOD_BO_FLAG_NO_MMAP),
             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &bo) == VK_SUCCESS) {
         util_dynarray_append(&dev->tls.bos, bo);
         cur = bo;
      } else {
         cur = NULL;
      }
   }

   if (cur)
      addr = cur->addr.dev;
   simple_mtx_unlock(&dev->tls.lock);

   if (!addr)
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return addr;
}

static void
emit_tls(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   unsigned core_id_range = pan_query_core_id_range(&phys_dev->kmod.dev->props);

   if (cmdbuf->state.tls.info.tls.size) {
      unsigned thread_tls_alloc =
         pan_query_thread_tls_alloc(&phys_dev->kmod.dev->props);
      unsigned size = pan_get_total_stack_size(cmdbuf->state.tls.info.tls.size,
                                               thread_tls_alloc, core_id_range);

      cmdbuf->state.tls.info.tls.ptr = get_device_tls(cmdbuf, size);
   }

   assert(!cmdbuf->state.tls.info.wls.size);

   if (cmdbuf->state.tls.desc.cpu) {
      GENX(pan_emit_tls)(&cmdbuf->state.tls.info, cmdbuf->state.tls.desc.cpu);
   }
}

/**
 * Write all sync point updates to seqno registers and reset the relative sync
 * points to 0.
 */
static void
flush_sync_points(struct panvk_cmd_buffer *cmdbuf)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);

      if (!cs_is_valid(b)) {
         vk_command_buffer_set_error(&cmdbuf->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }

      cs_update_progress_seqno(b) {
         for (uint32_t j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
            uint32_t rel_sync_point = cmdbuf->state.cs[j].relative_sync_point;

            if (!rel_sync_point)
               continue;

            cs_add_imm64(b, cs_progress_seqno_reg(b, j),
                         cs_progress_seqno_reg(b, j), rel_sync_point);
         }
      }
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)
      cmdbuf->state.cs[i].relative_sync_point = 0;
}

static void
finish_cs(struct panvk_cmd_buffer *cmdbuf, uint32_t subqueue)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, subqueue);

   panvk_per_arch(kbase_mark_progress)(
      cmdbuf, subqueue, PANVK_KBASE_PROGRESS_FINISH_BEFORE_WAIT);
   cs_wait_slots(b, dev->csf.sb.all_mask);
   panvk_per_arch(kbase_mark_progress)(
      cmdbuf, subqueue, PANVK_KBASE_PROGRESS_FINISH_AFTER_WAIT);

   /* save CS error if non-zero */
   if (cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      struct cs_index sync_addr = cs_scratch_reg64(b, 0);
      struct cs_index error = cs_scratch_reg32(b, 2);

      cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),
                   offsetof(struct panvk_cs_subqueue_context, syncobjs));
      cs_load32_to(b, error, sync_addr,
                   sizeof(struct panvk_cs_sync64) * subqueue +
                      offsetof(struct panvk_cs_sync64, error));
      cs_flush_loads(b);

      cs_if(b, MALI_CS_CONDITION_NEQUAL, error) {
         cs_store32(b, error, cs_subqueue_ctx_reg(b),
                    offsetof(struct panvk_cs_subqueue_context, last_error));
         cs_flush_stores(b);
      }
   }

   /* We need a clean because descriptor/CS memory can be returned to the
    * command pool where they get recycled. If we don't clean dirty cache lines,
    * those cache lines might get evicted asynchronously and their content
    * pushed back to main memory after the CPU has written new stuff there. */
   struct cs_index flush_id = cs_scratch_reg32(b, 0);

   panvk_per_arch(panvk_instr_begin_work)(subqueue, cmdbuf,
                                          PANVK_INSTR_WORK_TYPE_FLUSH_CACHE);
   cs_move32_to(b, flush_id, 0);
   cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,
                   MALI_CS_OTHER_FLUSH_MODE_NONE, flush_id,
                   cs_defer(SB_IMM_MASK, SB_ID(IMM_FLUSH)));
   cs_wait_slot(b, SB_ID(IMM_FLUSH));
   struct panvk_instr_end_args instr_info_flush = {
      .flush_cache = {
         .l2 = MALI_CS_FLUSH_MODE_CLEAN,
         .lsc = MALI_CS_FLUSH_MODE_CLEAN,
         .other = MALI_CS_OTHER_FLUSH_MODE_NONE,
      }};
   panvk_per_arch(panvk_instr_end_work)(
      subqueue, cmdbuf, PANVK_INSTR_WORK_TYPE_FLUSH_CACHE, &instr_info_flush);

   /* If this is a secondary command buffer, we don't poison the reg file to
    * preserve the render pass context. We also don't poison the reg file if the
    * last render pass was suspended. In practice we could preserve only the
    * registers that matter, but this is a debug feature so let's keep things
    * simple with this all-or-nothing approach. */
   if (PANVK_DEBUG(CS) &&
       cmdbuf->vk.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
       !cmdbuf->state.gfx.render.suspended) {
      static const reg_perm_cb_t ctx_reg_perms[PANVK_SUBQUEUE_COUNT] = {
         [PANVK_SUBQUEUE_VERTEX_TILER] = panvk_cs_vt_ctx_reg_perm,
         [PANVK_SUBQUEUE_FRAGMENT] = panvk_cs_frag_ctx_reg_perm,
         [PANVK_SUBQUEUE_COMPUTE] = panvk_cs_compute_ctx_reg_perm,
      };
      static const reg_perm_cb_t default_reg_perms[PANVK_SUBQUEUE_COUNT] = {
         [PANVK_SUBQUEUE_VERTEX_TILER] = panvk_cs_vt_reg_perm,
         [PANVK_SUBQUEUE_FRAGMENT] = panvk_cs_frag_reg_perm,
         [PANVK_SUBQUEUE_COMPUTE] = panvk_cs_compute_reg_perm,
      };
      const reg_perm_cb_t ctx_perm = ctx_reg_perms[subqueue];
      const reg_perm_cb_t default_perm = default_reg_perms[subqueue];

      cs_update_cmdbuf_regs(b) {
         /* Poison all cmdbuf registers to make sure we don't inherit state from
          * a previously executed cmdbuf. */
         for (uint32_t i = 0; i <= PANVK_CS_REG_SCRATCH_END; i++) {
            /* If the default or context access has the WR bit, we're good to
             * poison, otherwise we assume this is a RO register that's expected
             * to stay zero. */
             if ((default_perm(b, i) | ctx_perm(b, i)) & CS_REG_WR)
                cs_move32_to(b, cs_reg32(b, i), 0xdead | i << 24);
         }
      }
   }

   struct panvk_instr_end_args instr_info_cmdbuf = {.cmdbuf = {
                                                       .flags = cmdbuf->flags,
                                                    }};
   panvk_per_arch(panvk_instr_end_work)(
      subqueue, cmdbuf, PANVK_INSTR_WORK_TYPE_CMDBUF, &instr_info_cmdbuf);

   panvk_per_arch(kbase_mark_progress)(
      cmdbuf, subqueue, PANVK_KBASE_PROGRESS_CMDBUF_DONE);
   cs_end(&cmdbuf->state.cs[subqueue].builder);
}

static void
finish_queries(struct panvk_cmd_buffer *cmdbuf)
{
   enum panvk_subqueue_id signal_queue = PANVK_QUERY_TS_INFO_SUBQUEUE;

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, signal_queue);
   struct cs_index next = cs_scratch_reg64(b, 6);
   struct cs_index syncobj = cs_scratch_reg64(b, 2);
   struct cs_index signal_val = cs_scratch_reg32(b, 4);

   cs_load64_to(
      b, next, cs_subqueue_ctx_reg(b),
      offsetof(struct panvk_cs_subqueue_context, render.ts_done_chain.head));

   /* If there are queries to signal, wait for other subqueues before
    * signalling the syncobjs. */
   struct panvk_cs_deps deps = {0};
   deps.dst[signal_queue].wait_subqueue_mask =
      BITFIELD_MASK(PANVK_SUBQUEUE_COUNT) & ~BITFIELD_BIT(signal_queue);
   deps.dst[signal_queue].conditional = true;
   deps.dst[signal_queue].cond_value = next;
   deps.dst[signal_queue].cond = MALI_CS_CONDITION_NEQUAL;
   /* Wait for DEFERRED_SYNC in addition to LS so that we don't overtake the
    * deferred SYNC_ADDs added after frag jobs. */
   u_foreach_bit(i, deps.dst[signal_queue].wait_subqueue_mask)
      deps.src[i].wait_sb_mask = SB_MASK(LS) | SB_MASK(DEFERRED_SYNC);
   panvk_per_arch(emit_barrier)(cmdbuf, deps);

   cs_single_link_list_for_each_from(b, next, struct panvk_cs_timestamp_query,
                                     node) {
      cs_load64_to(b, syncobj, next,
                   offsetof(struct panvk_cs_timestamp_query, avail));

      cs_move32_to(b, signal_val, 1);
      cs_sync32_set(b, true, MALI_CS_SYNC_SCOPE_CSG, signal_val, syncobj,
                    cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_SYNC)));
   }

   cs_move64_to(b, next, 0);
   cs_store64(
      b, next, cs_subqueue_ctx_reg(b),
      offsetof(struct panvk_cs_subqueue_context, render.ts_done_chain.head));
   cs_flush_stores(b);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(EndCommandBuffer)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   panvk_cmd_rp_merge_flush_pending(cmdbuf);
   panvk_cmd_lazy_vt_flush_pending(cmdbuf, PANVK_LAZY_VT_END);

   /* Finishing queries requires a barrier. We don't want to do that more
    * often than necessary. At the end of a primary is usually enough.
    * Additionally, simultaneous use secondaries also need to flush if they
    * contain timestamp query writes to avoid adding the same node more than
    * once into panvk_cs_subqueue_context::render.ts_chain. */
   const bool sim_use_sec_with_ts =
      cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
      (cmdbuf->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) &&
      cmdbuf->state.contains_timestamp_queries;
   if (cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
       unlikely(sim_use_sec_with_ts))
      finish_queries(cmdbuf);

   emit_tls(cmdbuf);
   flush_sync_points(cmdbuf);

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++) {
      struct cs_builder *b = &cmdbuf->state.cs[i].builder;

      if (!cs_is_valid(b)) {
         vk_command_buffer_set_error(&cmdbuf->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
      } else if (!cs_is_empty(b)) {
         /* An empty stream is never submitted (see
          * gpu_queue_submit_init_storage()), so there is nothing to wait for
          * or flush in it.  Skipping finish_cs() avoids emitting a wait,
          * an error check and a cache clean into a command stream that is
          * discarded. */
         finish_cs(cmdbuf, i);
      }
   }

   panvk_pool_flush_maps(&cmdbuf->cs_pool);
   panvk_pool_flush_maps(&cmdbuf->desc_pool);

   return vk_command_buffer_end(&cmdbuf->vk);
}

/* The conditional rendering predicate is only read by the vertex/tiler
 * subqueue (draws, fullscreen meta draws) and the compute subqueue
 * (dispatches), never by the fragment subqueue, yet the fragment subqueue's
 * stage set includes CONDITIONAL_RENDERING.  vkd3d-proton writes predicates
 * with compute and barriers them to the conditional rendering stage for
 * every predicated draw, which made the fragment subqueue wait for the
 * compute subqueue each time (RDR1: thousands of waits per second).
 * PANVK_CR_FRAG_WAIT=1 restores the old behaviour; with
 * PANVK_CR_FRAG_WAIT_FILE=<path> the fix is active only while that file
 * exists (checked once a second), for same-session A/B runs. */
static bool
cr_skip_frag(void)
{
   static int old = -1;
   static const char *toggle;
   static int64_t last_check;
   static bool on;
   if (old < 0) {
      old = debug_get_bool_option("PANVK_CR_FRAG_WAIT", false);
      toggle = debug_get_option("PANVK_CR_FRAG_WAIT_FILE", "");
   }
   if (!toggle[0])
      return !old;
   const int64_t now = os_time_get_nano();
   if (now - p_atomic_read(&last_check) > 1000000000ll) {
      p_atomic_set(&last_check, now);
      on = access(toggle, F_OK) == 0;
   }
   return on;
}

static VkPipelineStageFlags2
barrier_subqueue_stages(enum panvk_subqueue_id subqueue)
{
   VkPipelineStageFlags2 stages = panvk_get_subqueue_stages(subqueue);
   if (subqueue == PANVK_SUBQUEUE_FRAGMENT && cr_skip_frag())
      stages &= ~VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
   return stages;
}

static void
add_execution_dependency(uint32_t wait_masks[static PANVK_SUBQUEUE_COUNT],
                         VkPipelineStageFlags2 src_stages,
                         VkPipelineStageFlags2 dst_stages, bool gpu_prerast)
{
   /* convert stages to subqueues */
   uint32_t src_subqueues = 0;
   uint32_t dst_subqueues = 0;
   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      const VkPipelineStageFlags2 subqueue_stages = barrier_subqueue_stages(i);
      if (src_stages & subqueue_stages)
         src_subqueues |= BITFIELD_BIT(i);
      if (dst_stages & subqueue_stages)
         dst_subqueues |= BITFIELD_BIT(i);
   }

   /* gpu_prerast runs the lowered VS on the COMPUTE subqueue, which reads
    * indirect params, vertex inputs and VS resources.  Make it a consumer of
    * those stages so GPU-written inputs are visible before the dispatch. */
   if (gpu_prerast &&
       (dst_stages & (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                      VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                      VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                      VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
                      VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                      VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT)))
      dst_subqueues |= BITFIELD_BIT(PANVK_SUBQUEUE_COMPUTE);

   const bool dst_host = dst_stages & VK_PIPELINE_STAGE_2_HOST_BIT;

   /* nothing to wait */
   if (!src_subqueues || (!dst_subqueues && !dst_host))
      return;

   u_foreach_bit(i, dst_subqueues) {
      /* each dst subqueue should wait for all src subqueues */
      uint32_t wait_mask = src_subqueues;

      switch (i) {
      case PANVK_SUBQUEUE_VERTEX_TILER:
         /* Indirect draw buffers are read from the command stream, and
          * load/store operations are synchronized with the LS scoreboard
          * immediately after the read, so no need to wait in that case.
          */
         if ((src_stages & panvk_get_subqueue_stages(i)) ==
             VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT)
            wait_mask &= ~BITFIELD_BIT(i);
         break;
      case PANVK_SUBQUEUE_FRAGMENT:
         /* The fragment subqueue always waits for the tiler subqueue already.
          * Explicit waits can be skipped.
          */
         wait_mask &= ~BITFIELD_BIT(PANVK_SUBQUEUE_VERTEX_TILER);
         break;
      default:
         break;
      }

      wait_masks[i] |= wait_mask;
   }

   /* The host does not wait for src subqueues.  All src subqueues should
    * self-wait instead.
    *
    * Also, our callers currently expect src subqueues to self-wait when there
    * are dst subqueues.  Until that changes, make all src subqueues self-wait.
    */
   if (dst_host || dst_subqueues) {
      u_foreach_bit(i, src_subqueues)
         wait_masks[i] |= BITFIELD_BIT(i);
   }
}

static void
add_memory_dependency(struct panvk_cache_flush_info *cache_flush,
                      VkAccessFlags2 src_access, VkAccessFlags2 dst_access)
{
   /* Note on the cache organization:
    *
    * - L2 cache is unified, so all changes to this cache are automatically
    *   visible to all GPU sub-components (shader cores, tiler, ...). This
    *   means we only need to flush when the host (AKA CPU) is involved.
    * - LS caches (which are basically just read-write L1 caches) are coherent
    *   with each other and with the L2 cache, so again, we only need to flush
    *   when the host is involved.
    * - Other read-only L1 caches (like the ones in front of the texture unit)
    *   are not coherent with the LS or L2 caches, and thus need to be
    *   invalidated any time a write happens.
    *
    * Translating to the Vulkan memory model:
    *
    * - The device domain is the L2 cache.
    * - An availability operation from device writes to the device domain is
    *   nop.
    * - A visibility operation from the device domain to device accesses that
    *   are coherent with L2/LS is nop.
    * - A visibility operation from the device domain to device accesses that
    *   are incoherent with L2/LS invalidates the other RO L1 caches.
    * - A host-to-device domain operation invalidates all caches.
    * - A device-to-host domain operation flushes L2/LS.
    */
   const VkAccessFlags2 ro_l1_access =
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
      VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;

   /* visibility op */
   if (dst_access & ro_l1_access)
      cache_flush->others |= MALI_CS_OTHER_FLUSH_MODE_INVALIDATE;

   /* host-to-device domain op */
   if (src_access & VK_ACCESS_2_HOST_WRITE_BIT) {
      cache_flush->l2 |= MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE;
      cache_flush->lsc |= MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE;
      cache_flush->others |= MALI_CS_OTHER_FLUSH_MODE_INVALIDATE;
   }

   /* device-to-host domain op */
   if (dst_access & (VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT)) {
      cache_flush->l2 |= MALI_CS_FLUSH_MODE_CLEAN;
      cache_flush->lsc |= MALI_CS_FLUSH_MODE_CLEAN;
   }
}

static void
collect_cache_flush_info(enum panvk_subqueue_id subqueue,
                         struct panvk_cache_flush_info *cache_flush,
                         VkAccessFlags2 src_access, VkAccessFlags2 dst_access)
{
   /* limit access to the subqueue and host */
   const VkPipelineStageFlags2 subqueue_stages =
      panvk_get_subqueue_stages(subqueue) | VK_PIPELINE_STAGE_2_HOST_BIT;
   src_access = vk_filter_src_access_flags2(subqueue_stages, src_access);
   dst_access = vk_filter_dst_access_flags2(subqueue_stages, dst_access);

   add_memory_dependency(cache_flush, src_access, dst_access);
}

/* Lazy waits on the fragment subqueue, on by default (PANVK_LAZY_VTWAIT=0
 * disables them, PANVK_LAZY_CWAIT=0 only the compute subqueue ones).
 *
 * vkd3d-proton transitions render targets to shader-read states with every
 * non-pixel shader stage in the destination scope (D3D12
 * NON_PIXEL_SHADER_RESOURCE), so after nearly every pass the vertex/tiler
 * and compute subqueues waited for the fragment subqueue to go idle: the
 * next pass could not be tiled while the previous one was shaded, and
 * vkd3d's per-draw predicate dispatches, which the next draws wait for,
 * waited for the fragment work too (RDR1: ~2300 + ~2800 waits/s, the
 * fragment subqueue idle a third of the time).
 *
 * When the fragment-side writes in the source scope are attachment writes
 * only, and the waiting subqueue's part of the destination scope only
 * reads, the only work on that subqueue that can observe those writes is a
 * shader sampling or loading an image (attachments are images; memory
 * aliasing between an attachment and a buffer is not covered), plus, for
 * the execution dependency, a shader writing memory the fragment work may
 * have read (write-after-read).  The wait is then recorded instead of
 * emitted (the fragment subqueue still signals), and emitted before the
 * next draw or dispatch on that subqueue whose shaders read images or write
 * memory, before geometry/tessellation/gpu_prerast draws and internal
 * compute work, before events, meta operations and secondary execution,
 * and at the end of the command buffer.  Under vkd3d-proton and DXVK any
 * barrier that orders reads before later writes has write access in its
 * destination scope (see cmd_d3d12_semantics), so shader writes do not need
 * the deferred wait there.  Transfer and query
 * work never observes those image writes.  A later barrier that makes
 * another subqueue wait for a subqueue with a deferred wait hands the
 * deferred wait over when its part of that barrier only reads, and emits
 * it otherwise, so execution dependency chains still hold.
 *
 * PANVK_LAZY_VTWAIT_FILE / PANVK_LAZY_CWAIT_FILE=<path>: enabled only while
 * that file exists (checked once a second), for same-session A/B runs. */
static bool
lazy_vt_enabled(void)
{
   static int enabled = -1;
   static const char *toggle;
   static int64_t last_check;
   static bool on;
   if (enabled < 0) {
      enabled = debug_get_bool_option("PANVK_LAZY_VTWAIT", true);
      toggle = debug_get_option("PANVK_LAZY_VTWAIT_FILE", "");
   }
   if (!toggle[0])
      return enabled;
   const int64_t now = os_time_get_nano();
   if (now - p_atomic_read(&last_check) > 1000000000ll) {
      p_atomic_set(&last_check, now);
      on = access(toggle, F_OK) == 0;
   }
   return on;
}

static bool
lazy_c_enabled(void)
{
   static int enabled = -1;
   static const char *toggle;
   static int64_t last_check;
   static bool on;
   if (enabled < 0) {
      enabled = debug_get_bool_option("PANVK_LAZY_CWAIT", true);
      toggle = debug_get_option("PANVK_LAZY_CWAIT_FILE", "");
   }
   if (!toggle[0])
      return enabled;
   const int64_t now = os_time_get_nano();
   if (now - p_atomic_read(&last_check) > 1000000000ll) {
      p_atomic_set(&last_check, now);
      on = access(toggle, F_OK) == 0;
   }
   return on;
}

bool
panvk_per_arch(cmd_d3d12_semantics)(const struct panvk_cmd_buffer *cmdbuf)
{
   static int d3d12 = -1;
   if (d3d12 < 0) {
      const struct vk_instance *instance =
         cmdbuf->vk.base.device->physical->instance;
      const char *engine = instance->app_info.engine_name;
      d3d12 = engine && (!strcmp(engine, "vkd3d") || !strcmp(engine, "DXVK"));
      mesa_logi("panvk: engine '%s': %s barrier write rules", engine ? engine : "",
                d3d12 ? "D3D translation-layer" : "strict Vulkan");
   }
   return d3d12;
}

/* VkAccessFlagBits2 values are const variables, not constant expressions. */
#define lazy_vt_read_access                                                    \
  (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | \
   VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT | \
   VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | \
   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | \
   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | \
   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT | \
   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | \
   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | \
   VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR | \
   VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT | \
   VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | \
   VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT | \
   VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT | \
   VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR | \
   VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT | \
   VK_ACCESS_2_MICROMAP_READ_BIT_EXT)

/* Whether the fragment-side writes of the source scope are attachment
 * writes only. */
static bool
lazy_frag_src_ok(struct panvk_sync_scope src)
{
   const VkAccessFlags2 att_writes =
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   const VkAccessFlags2 frag_src = vk_filter_src_access_flags2(
      panvk_get_subqueue_stages(PANVK_SUBQUEUE_FRAGMENT), src.access);

   return !(frag_src & ~(att_writes | lazy_vt_read_access));
}

/* Whether subqueue's part of the destination scope only reads. */
static bool
lazy_dst_read_only(struct panvk_sync_scope dst, enum panvk_subqueue_id subqueue)
{
   return !(vk_filter_dst_access_flags2(panvk_get_subqueue_stages(subqueue),
                                        dst.access) &
            ~lazy_vt_read_access);
}

static bool
lazy_vt_eligible(struct panvk_sync_scope src, struct panvk_sync_scope dst)
{
   const VkAccessFlags2 att_writes =
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   const VkAccessFlags2 frag_src = vk_filter_src_access_flags2(
      panvk_get_subqueue_stages(PANVK_SUBQUEUE_FRAGMENT), src.access);
   const VkAccessFlags2 vt_dst = vk_filter_dst_access_flags2(
      panvk_get_subqueue_stages(PANVK_SUBQUEUE_VERTEX_TILER), dst.access);

   const bool ok = !(frag_src & ~(att_writes | lazy_vt_read_access)) &&
                   !(vt_dst & ~lazy_vt_read_access);

   /* Debug: PANVK_LAZY_TRACE=N logs the first N rejected barriers. */
   static int64_t trace = -1, skip;
   static uint32_t traced;
   if (trace < 0) {
      trace = debug_get_num_option("PANVK_LAZY_TRACE", 0);
      skip = debug_get_num_option("PANVK_LAZY_TRACE_SKIP", 0);
   }
   uint32_t n;
   if (!ok && trace && (n = p_atomic_inc_return(&traced)) > skip &&
       n <= skip + trace)
      mesa_logi("lazytrace: src stages %" PRIx64 " access %" PRIx64
                " dst stages %" PRIx64 " access %" PRIx64
                " | frag_src %" PRIx64 " vt_dst %" PRIx64,
                (uint64_t)src.stages, (uint64_t)src.access,
                (uint64_t)dst.stages, (uint64_t)dst.access,
                (uint64_t)frag_src, (uint64_t)vt_dst);

   return ok && lazy_vt_enabled();
}

static void
collect_cs_deps(struct panvk_cmd_buffer *cmdbuf, const VkDependencyInfo *info,
                struct panvk_sync_scope src, struct panvk_sync_scope dst,
                struct panvk_cs_deps *deps)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   uint32_t wait_masks[PANVK_SUBQUEUE_COUNT] = {0};
   add_execution_dependency(
      wait_masks, src.stages, dst.stages,
      panvk_priv_mem_check_alloc(dev->gpu_prerast_arena) ||
         panvk_priv_mem_check_alloc(dev->gpu_prerast_tess_arena));

   if (cmdbuf->state.gfx.render.tiler || inherits_render_ctx(cmdbuf)) {
      if (info->dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT) {
         /* Instead of doing an actual fragment job dependency, use a FB
          * barrier instead.
          */
         deps->needs_fb_barrier = true;
         wait_masks[PANVK_SUBQUEUE_FRAGMENT] &=
            ~BITFIELD_BIT(PANVK_SUBQUEUE_FRAGMENT);
      }

      /* From the Vulkan 1.4.335 spec:
       *
       *    VUID-vkCmdPipelineBarrier-dependencyFlags-07891
       *
       *    "If vkCmdPipelineBarrier is called within a render pass
       *    instance, and the source stage masks of any memory barriers
       *    include framebuffer-space stages, then dependencyFlags must
       *    include VK_DEPENDENCY_BY_REGION_BIT"
       */
      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         assert(!(wait_masks[i] & BITFIELD_BIT(PANVK_SUBQUEUE_FRAGMENT)));

      /* skip the tiler subqueue self-wait because we use the same
       * scoreboard slot for the idvs jobs
       */
      wait_masks[PANVK_SUBQUEUE_VERTEX_TILER] &=
         ~BITFIELD_BIT(PANVK_SUBQUEUE_VERTEX_TILER);
   }

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (wait_masks[i] & BITFIELD_BIT(i)) {
         /* We need to self-wait for all previously submitted jobs, and given
          * the iterator scoreboard is a moving target, we just wait for the
          * whole dynamic scoreboard range.
          */
         deps->src[i].wait_sb_mask |= dev->csf.sb.all_iters_mask;
      }

      collect_cache_flush_info(i, &deps->src[i].cache_flush, src.access,
                               dst.access);

      deps->dst[i].wait_subqueue_mask |= wait_masks[i];
   }

   /* add_cs_deps() calls this once per barrier of the dependency, so a
    * deferrable wait is recorded in frag_lazy_mask and a barrier that makes
    * the same subqueue wait without allowing it vetoes it. */
   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (i == PANVK_SUBQUEUE_FRAGMENT)
         continue;
      if (!lazy_dst_read_only(dst, i))
         deps->dst_writes_mask |= BITFIELD_BIT(i);
      if (!(wait_masks[i] & BITFIELD_BIT(PANVK_SUBQUEUE_FRAGMENT)))
         continue;

      bool ok;
      if (i == PANVK_SUBQUEUE_VERTEX_TILER)
         ok = lazy_vt_eligible(src, dst);
      else
         ok = lazy_frag_src_ok(src) && lazy_dst_read_only(dst, i) &&
              lazy_vt_enabled() && lazy_c_enabled();

      if (ok)
         deps->frag_lazy_mask |= BITFIELD_BIT(i);
      else
         deps->frag_lazy_veto |= BITFIELD_BIT(i);
   }

   /* Debug: PANVK_BAR_TRACE=N logs N barrier scopes that create one of the
    * cross-subqueue waits in PANVK_BAR_TRACE_EDGES (bit 3 * waiter +
    * waited_on, default all), after PANVK_BAR_TRACE_SKIP matches. */
   static int64_t bt_limit = -1, bt_skip, bt_edges;
   static uint32_t bt_count;
   if (bt_limit < 0) {
      bt_limit = debug_get_num_option("PANVK_BAR_TRACE", 0);
      bt_skip = debug_get_num_option("PANVK_BAR_TRACE_SKIP", 0);
      bt_edges = debug_get_num_option("PANVK_BAR_TRACE_EDGES", 0x1ff);
   }
   if (bt_limit) {
      uint32_t edges = 0;
      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         edges |= (wait_masks[i] & ~BITFIELD_BIT(i)) << (PANVK_SUBQUEUE_COUNT * i);
      uint32_t n;
      if ((edges & bt_edges) && (n = p_atomic_inc_return(&bt_count)) > bt_skip &&
          n <= bt_skip + bt_limit)
         mesa_logi("bartrace: edges %03x src %" PRIx64 "/%" PRIx64
                   " dst %" PRIx64 "/%" PRIx64 " mem %u buf %u img %u%s",
                   edges, (uint64_t)src.stages, (uint64_t)src.access,
                   (uint64_t)dst.stages, (uint64_t)dst.access,
                   info->memoryBarrierCount, info->bufferMemoryBarrierCount,
                   info->imageMemoryBarrierCount,
                   cmdbuf->state.gfx.render.tiler ? " in-pass" : "");
   }
}

static void
normalize_dependency(struct panvk_sync_scope *src,
                     struct panvk_sync_scope *dst,
                     uint32_t src_qfi, uint32_t dst_qfi)
{
   /* Perform queue family ownership transfer if src and dst are unequal. */
   if (src_qfi != dst_qfi) {
      /* queue family acquire operation */
      switch (src_qfi) {
      case VK_QUEUE_FAMILY_EXTERNAL:
         /* no execution dependency and no availability operation */
         *src = (struct panvk_sync_scope){VK_PIPELINE_STAGE_2_NONE,
                                          VK_ACCESS_2_NONE};
         break;
      case VK_QUEUE_FAMILY_FOREIGN_EXT:
         /* treat the foreign queue as the host */
         *src = (struct panvk_sync_scope){VK_PIPELINE_STAGE_2_HOST_BIT,
                                          VK_ACCESS_2_HOST_WRITE_BIT};
         break;
      default:
         break;
      }

      /* queue family release operation */
      switch (dst_qfi) {
      case VK_QUEUE_FAMILY_EXTERNAL:
         /* no execution dependency and no visibility operation */
         *dst = (struct panvk_sync_scope){VK_PIPELINE_STAGE_2_NONE,
                                          VK_ACCESS_2_NONE};
         break;
      case VK_QUEUE_FAMILY_FOREIGN_EXT:
         /* treat the foreign queue as the host */
         *dst = (struct panvk_sync_scope){VK_PIPELINE_STAGE_2_HOST_BIT,
                                          VK_ACCESS_2_HOST_WRITE_BIT};
         break;
      default:
         break;
      }
   }

   src->stages = vk_expand_src_stage_flags2(src->stages);
   src->access = vk_filter_src_access_flags2(src->stages, src->access);
   dst->stages = vk_expand_dst_stage_flags2(dst->stages);
   dst->access = vk_filter_dst_access_flags2(dst->stages, dst->access);
}

void
panvk_per_arch(add_cs_deps)(struct panvk_cmd_buffer *cmdbuf,
                            const VkDependencyInfo *in,
                            struct panvk_cs_deps *out,
                            bool is_set_event)
{
   bool is_asymmetric_event =
      is_set_event &&
      in->dependencyFlags & VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR;

   /* As per Vulkan spec, this should look like a vkCmdSetEvent */
   assert(!is_asymmetric_event ||
          (in->memoryBarrierCount == 1 && in->bufferMemoryBarrierCount == 0 &&
           in->imageMemoryBarrierCount == 0));

   for (uint32_t i = 0; i < in->memoryBarrierCount; i++) {
      const VkMemoryBarrier2 *barrier = &in->pMemoryBarriers[i];
      struct panvk_sync_scope src = {barrier->srcStageMask, barrier->srcAccessMask};
      struct panvk_sync_scope dst = {barrier->dstStageMask, barrier->dstAccessMask};

      /* In case of asymmetric event, we need to use the src stages mask just
       * like vkCmdSetEvent */
      if (is_asymmetric_event)
         dst.stages = src.stages;

      normalize_dependency(&src, &dst, VK_QUEUE_FAMILY_IGNORED,
                           VK_QUEUE_FAMILY_IGNORED);

      collect_cs_deps(cmdbuf, in, src, dst, out);
   }

   for (uint32_t i = 0; i < in->bufferMemoryBarrierCount; i++) {
      const VkBufferMemoryBarrier2 *barrier = &in->pBufferMemoryBarriers[i];
      VK_FROM_HANDLE(panvk_buffer, buffer, barrier->buffer);
      struct panvk_sync_scope src = {barrier->srcStageMask, barrier->srcAccessMask};
      struct panvk_sync_scope dst = {barrier->dstStageMask, barrier->dstAccessMask};
      normalize_dependency(&src, &dst, barrier->srcQueueFamilyIndex,
                           barrier->dstQueueFamilyIndex);

      if (!(buffer->vk.usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
         dst.access &= ~(VK_ACCESS_2_SHADER_READ_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

      collect_cs_deps(cmdbuf, in, src, dst, out);
   }

   for (uint32_t i = 0; i < in->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *barrier = &in->pImageMemoryBarriers[i];
      struct panvk_sync_scope src = {barrier->srcStageMask, barrier->srcAccessMask};
      struct panvk_sync_scope dst = {barrier->dstStageMask, barrier->dstAccessMask};
      normalize_dependency(&src, &dst, barrier->srcQueueFamilyIndex,
                           barrier->dstQueueFamilyIndex);

      collect_cs_deps(cmdbuf, in, src, dst, out);
   }
}

static void
emit_barrier_insert_waits(struct cs_builder *b, struct panvk_cmd_buffer *cmdbuf,
                          struct panvk_cs_deps *deps, enum panvk_subqueue_id i,
                          struct cs_index tmp_regs, uint32_t skip_mask)
{
   assert(tmp_regs.size == 4);
   u_foreach_bit(j, deps->dst[i].wait_subqueue_mask & ~skip_mask) {
      struct panvk_cs_state *cs_state = &cmdbuf->state.cs[j];
      struct cs_index sync_addr = cs_reg64(b, tmp_regs.reg);
      struct cs_index wait_val = cs_reg64(b, tmp_regs.reg + 2);

      cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),
                   offsetof(struct panvk_cs_subqueue_context, syncobjs));
      cs_add_imm64(b, sync_addr, sync_addr, sizeof(struct panvk_cs_sync64) * j);

      cs_add_imm64(b, wait_val, cs_progress_seqno_reg(b, j),
                   cs_state->relative_sync_point);

      panvk_instr_sync64_wait(cmdbuf, i, false, MALI_CS_CONDITION_GREATER,
                              wait_val, sync_addr);
   }
}

/* Debug (PANVK_PASS_STATS): per-second counts of barrier waits.  self[i]:
 * subqueue i drains its own scoreboard; edge[i][j]: subqueue i waits for
 * subqueue j; flush[i]: subqueue i flushes caches.  0=VT 1=FRAG 2=COMP. */
static uint32_t bstat_n, bstat_self[PANVK_SUBQUEUE_COUNT],
   bstat_flush[PANVK_SUBQUEUE_COUNT],
   bstat_edge[PANVK_SUBQUEUE_COUNT][PANVK_SUBQUEUE_COUNT],
   bstat_lazy[PANVK_SUBQUEUE_COUNT], bstat_lazy_handover,
   bstat_lazy_flush[PANVK_LAZY_VT_REASONS],
   bstat_disp[4]; /* dispatches by reads_images | writes_memory << 1 */

/* The barrier and dispatch counters are only kept with PANVK_PASS_STATS=1. */
static bool
bstats_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0)
      enabled = debug_get_bool_option("PANVK_PASS_STATS", false);
   return enabled;
}

void
panvk_per_arch(dispatch_stats_record)(bool reads_images, bool writes_memory)
{
   if (bstats_enabled())
      p_atomic_inc(&bstat_disp[reads_images | (writes_memory << 1)]);
}

void
panvk_per_arch(barrier_stats_log)(void)
{
   static const char sq[] = "VFC";
   char buf[512];
   int o = snprintf(buf, sizeof(buf), "barriers:%u", p_atomic_xchg(&bstat_n, 0));
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      o += snprintf(buf + o, sizeof(buf) - o, " self%c:%u flush%c:%u", sq[i],
                    p_atomic_xchg(&bstat_self[i], 0), sq[i],
                    p_atomic_xchg(&bstat_flush[i], 0));
   }
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++)
         if (i != j)
            o += snprintf(buf + o, sizeof(buf) - o, " %c<-%c:%u", sq[i], sq[j],
                          p_atomic_xchg(&bstat_edge[i][j], 0));
   static const char *const why[PANVK_LAZY_VT_REASONS] = {
      "draw", "barrier", "meta", "event", "query", "end", "exec", "marker",
      "dispatch",
   };
   o += snprintf(buf + o, sizeof(buf) - o,
                 " lazyV:%u lazyC:%u handover:%u flush",
                 p_atomic_xchg(&bstat_lazy[PANVK_SUBQUEUE_VERTEX_TILER], 0),
                 p_atomic_xchg(&bstat_lazy[PANVK_SUBQUEUE_COMPUTE], 0),
                 p_atomic_xchg(&bstat_lazy_handover, 0));
   for (unsigned r = 0; r < PANVK_LAZY_VT_REASONS; r++)
      o += snprintf(buf + o, sizeof(buf) - o, " %s:%u", why[r],
                    p_atomic_xchg(&bstat_lazy_flush[r], 0));
   o += snprintf(buf + o, sizeof(buf) - o,
                 " | disp plain:%u img:%u wr:%u img+wr:%u",
                 p_atomic_xchg(&bstat_disp[0], 0), p_atomic_xchg(&bstat_disp[1], 0),
                 p_atomic_xchg(&bstat_disp[2], 0), p_atomic_xchg(&bstat_disp[3], 0));
   mesa_logi("bstats: %s", buf);
}

/* Debug, UNSAFE (may race): PANVK_DROP_WAITS=<mask> drops cross-subqueue
 * barrier waits, bit (3 * waiter + waited_on), e.g. 0x2 = VT waiting on
 * FRAG, to measure what each wait type costs.  With
 * PANVK_DROP_WAITS_FILE=<path>, only while that file exists (checked once
 * a second). */
static uint32_t
drop_waits_mask(void)
{
   static int64_t mask = -1, last_check;
   static const char *toggle;
   static bool on;
   if (mask < 0) {
      mask = debug_get_num_option("PANVK_DROP_WAITS", 0);
      toggle = debug_get_option("PANVK_DROP_WAITS_FILE", "");
   }
   if (!mask || !toggle[0])
      return mask;
   const int64_t now = os_time_get_nano();
   if (now - p_atomic_read(&last_check) > 1000000000ll) {
      p_atomic_set(&last_check, now);
      on = access(toggle, F_OK) == 0;
   }
   return on ? mask : 0;
}

/* Make subqueue wait for a fragment sync point recorded by a deferred
 * wait. */
static void
emit_lazy_frag_wait(struct panvk_cmd_buffer *cmdbuf,
                    enum panvk_subqueue_id subqueue, int32_t frag_sync_point)
{
   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, subqueue);
   struct cs_index sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index wait_val = cs_scratch_reg64(b, 2);

   cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),
                offsetof(struct panvk_cs_subqueue_context, syncobjs));
   cs_add_imm64(b, sync_addr, sync_addr,
                sizeof(struct panvk_cs_sync64) * PANVK_SUBQUEUE_FRAGMENT);
   cs_add_imm64(b, wait_val,
                cs_progress_seqno_reg(b, PANVK_SUBQUEUE_FRAGMENT),
                frag_sync_point);
   panvk_instr_sync64_wait(cmdbuf, subqueue, false,
                           MALI_CS_CONDITION_GREATER, wait_val, sync_addr);
}

void
panvk_per_arch(cmd_lazy_flush)(struct panvk_cmd_buffer *cmdbuf,
                               enum panvk_subqueue_id subqueue,
                               enum panvk_lazy_vt_flush_reason reason)
{
   if (!cmdbuf->state.lazy_wait[subqueue].pending)
      return;

   emit_lazy_frag_wait(cmdbuf, subqueue,
                       cmdbuf->state.lazy_wait[subqueue].frag_sync_point);
   cmdbuf->state.lazy_wait[subqueue].pending = false;
   p_atomic_inc(&bstat_lazy_flush[reason]);
}

/* Record a deferred wait for frag_sync_point (or a later point already
 * recorded). */
static void
lazy_record(struct panvk_cmd_buffer *cmdbuf, enum panvk_subqueue_id subqueue,
            int32_t frag_sync_point)
{
   if (!cmdbuf->state.lazy_wait[subqueue].pending ||
       cmdbuf->state.lazy_wait[subqueue].frag_sync_point < frag_sync_point)
      cmdbuf->state.lazy_wait[subqueue].frag_sync_point = frag_sync_point;
   cmdbuf->state.lazy_wait[subqueue].pending = true;
}

static void
emit_barrier_csf(struct panvk_cmd_buffer *cmdbuf, struct panvk_cs_deps deps)
{
   uint32_t wait_subqueue_mask = 0;
   uint32_t utrace_subqueue_mask = 0;

   const uint32_t drop = drop_waits_mask();
   if (drop) {
      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         deps.dst[i].wait_subqueue_mask &=
            ~((drop >> (PANVK_SUBQUEUE_COUNT * i)) &
              BITFIELD_MASK(PANVK_SUBQUEUE_COUNT));
   }
   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      /* no need to perform both types of waits on the same subqueue */
      if (deps.src[i].wait_sb_mask)
         deps.dst[i].wait_subqueue_mask &= ~BITFIELD_BIT(i);
      assert(!(deps.dst[i].wait_subqueue_mask & BITFIELD_BIT(i)));

      wait_subqueue_mask |= deps.dst[i].wait_subqueue_mask;

      if (deps.src[i].wait_sb_mask || deps.dst[i].wait_subqueue_mask ||
          !panvk_cache_flush_is_nop(&deps.src[i].cache_flush))
         utrace_subqueue_mask |= BITFIELD_BIT(i);
   }

   /* Work ordered after work of a subqueue with a deferred wait must also
    * be ordered after the fragment work that wait stands for (execution
    * dependency chains).  The fragment subqueue is past that point already.
    * Another subqueue whose part of this barrier only reads takes the
    * deferred wait over (same reasoning as for the original barrier);
    * otherwise it waits for that fragment point now.  The subqueue with
    * the deferred wait keeps it for its own later work. */
   uint32_t lazy_from = 0;
   for (uint32_t x = 0; x < PANVK_SUBQUEUE_COUNT; x++) {
      if (cmdbuf->state.lazy_wait[x].pending)
         lazy_from |= BITFIELD_BIT(x);
   }
   if (lazy_from) {
      int32_t points[PANVK_SUBQUEUE_COUNT];
      for (uint32_t x = 0; x < PANVK_SUBQUEUE_COUNT; x++)
         points[x] = cmdbuf->state.lazy_wait[x].frag_sync_point;

      for (uint32_t y = 0; y < PANVK_SUBQUEUE_COUNT; y++) {
         if (y == PANVK_SUBQUEUE_FRAGMENT)
            continue;
         u_foreach_bit(x, deps.dst[y].wait_subqueue_mask & lazy_from) {
            if (x == y)
               continue;
            if (!(deps.dst_writes_mask & BITFIELD_BIT(y)) &&
                (y == PANVK_SUBQUEUE_VERTEX_TILER ? lazy_vt_enabled()
                                                  : lazy_c_enabled())) {
               lazy_record(cmdbuf, y, points[x]);
               p_atomic_inc(&bstat_lazy_handover);
            } else {
               emit_lazy_frag_wait(cmdbuf, y, points[x]);
               p_atomic_inc(&bstat_lazy_flush[PANVK_LAZY_VT_BARRIER]);
            }
         }
      }
   }

   if (utrace_subqueue_mask && bstats_enabled()) {
      p_atomic_inc(&bstat_n);
      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
         if (deps.src[i].wait_sb_mask)
            p_atomic_inc(&bstat_self[i]);
         if (!panvk_cache_flush_is_nop(&deps.src[i].cache_flush))
            p_atomic_inc(&bstat_flush[i]);
         u_foreach_bit(j, deps.dst[i].wait_subqueue_mask)
            p_atomic_inc(&bstat_edge[i][j]);
      }
   }

   u_foreach_bit(i, utrace_subqueue_mask)
      panvk_per_arch(panvk_instr_begin_work)(i, cmdbuf,
                                             PANVK_INSTR_WORK_TYPE_BARRIER);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);
      struct panvk_cs_state *cs_state = &cmdbuf->state.cs[i];

      if (deps.src[i].wait_sb_mask)
         cs_wait_slots(b, deps.src[i].wait_sb_mask);

      struct panvk_cache_flush_info cache_flush = deps.src[i].cache_flush;
      if (!panvk_cache_flush_is_nop(&cache_flush)) {
         struct cs_index flush_id = cs_scratch_reg32(b, 0);

         panvk_per_arch(panvk_instr_begin_work)(
            i, cmdbuf, PANVK_INSTR_WORK_TYPE_FLUSH_CACHE);
         cs_move32_to(b, flush_id, 0);
         cs_flush_caches(b, cache_flush.l2, cache_flush.lsc, cache_flush.others,
                         flush_id, cs_defer(SB_IMM_MASK, SB_ID(IMM_FLUSH)));
         cs_wait_slot(b, SB_ID(IMM_FLUSH));
         struct panvk_instr_end_args instr_info_flush = {
            .flush_cache = {
               .l2 = cache_flush.l2,
               .lsc = cache_flush.lsc,
               .other = cache_flush.others,
            }};
         panvk_per_arch(panvk_instr_end_work)(
            i, cmdbuf, PANVK_INSTR_WORK_TYPE_FLUSH_CACHE, &instr_info_flush);
      }

      /* If no one waits on us, there's no point signaling the sync object. */
      if (wait_subqueue_mask & BITFIELD_BIT(i)) {
         struct cs_index sync_addr = cs_scratch_reg64(b, 0);
         struct cs_index add_val = cs_scratch_reg64(b, 2);

         assert(deps.src[i].wait_sb_mask);

         cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),
                      offsetof(struct panvk_cs_subqueue_context, syncobjs));
         cs_add_imm64(b, sync_addr, sync_addr,
                      sizeof(struct panvk_cs_sync64) * i);
         cs_move64_to(b, add_val, 1);
         panvk_instr_sync64_add(cmdbuf, i, true, MALI_CS_SYNC_SCOPE_CSG,
                                add_val, sync_addr, cs_now());
         ++cs_state->relative_sync_point;
      }
   }

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);
      struct cs_index tmp_regs = cs_scratch_reg_tuple(b, 0, 4);

      /* Deferred wait on the fragment subqueue: record the fragment sync
       * point just signaled instead of waiting for it. */
      uint32_t skip_mask = 0;
      if (i != PANVK_SUBQUEUE_FRAGMENT &&
          (deps.dst[i].wait_subqueue_mask &
           BITFIELD_BIT(PANVK_SUBQUEUE_FRAGMENT))) {
         const uint32_t lazy = deps.frag_lazy_mask & ~deps.frag_lazy_veto;
         if ((lazy & BITFIELD_BIT(i)) && !deps.dst[i].conditional) {
            skip_mask = BITFIELD_BIT(PANVK_SUBQUEUE_FRAGMENT);
            lazy_record(cmdbuf, i,
               cmdbuf->state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point);
            p_atomic_inc(&bstat_lazy[i]);
         } else {
            /* This wait covers any earlier deferred one. */
            cmdbuf->state.lazy_wait[i].pending = false;
         }
      }

      if (deps.dst[i].conditional) {
         assert(deps.dst[i].cond_value.reg >= tmp_regs.reg + tmp_regs.size ||
                deps.dst[i].cond_value.reg + deps.dst[i].cond_value.size <=
                   tmp_regs.reg);
         cs_if(b, deps.dst[i].cond, deps.dst[i].cond_value)
            emit_barrier_insert_waits(b, cmdbuf, &deps, i, tmp_regs, skip_mask);
      } else {
         emit_barrier_insert_waits(b, cmdbuf, &deps, i, tmp_regs, skip_mask);
      }
   }

   u_foreach_bit(i, utrace_subqueue_mask) {
      struct panvk_instr_end_args info = {
         .barrier = {
            .wait_sb_mask = deps.src[i].wait_sb_mask,
            .wait_subqueue_mask = deps.dst[i].wait_subqueue_mask,
         }};
      panvk_per_arch(panvk_instr_end_work)(
         i, cmdbuf, PANVK_INSTR_WORK_TYPE_BARRIER, &info);
   }
}

void
panvk_per_arch(emit_barrier)(struct panvk_cmd_buffer *cmdbuf,
                             struct panvk_cs_deps deps)
{
   /* Execute CRC invalidation after all source work and before releasing any
    * destination subqueue. One subqueue performs the state writes.
    */
   if (deps.crc.count) {
      struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
      deps.frag_lazy_mask = 0;
      uint32_t mask = deps.crc.dst_subqueue_mask;
      enum panvk_subqueue_id exec = panvk_crc_exec_subqueue(mask);

      u_foreach_bit(src, deps.crc.src_subqueue_mask) {
         deps.src[src].wait_sb_mask |= dev->csf.sb.all_iters_mask;

         if (src != exec) {
            deps.dst[exec].wait_subqueue_mask |= BITFIELD_BIT(src);
         }
      }

      struct panvk_cs_crc_deps crc = deps.crc;
      memset(&deps.crc, 0, sizeof(deps.crc));
      emit_barrier_csf(cmdbuf, deps);

      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, exec);
      for (uint32_t i = 0; i < crc.count; i++) {
         panvk_per_arch(cmd_invalidate_crc)(b, crc.addrs[i]);
      }

      uint32_t other_dst = crc.dst_subqueue_mask & ~BITFIELD_BIT(exec);
      if (other_dst) {
         struct panvk_cs_deps post = {0};
         post.src[exec].wait_sb_mask = SB_MASK(LS);

         u_foreach_bit(dst, other_dst) {
            post.dst[dst].wait_subqueue_mask = BITFIELD_BIT(exec);
         }

         emit_barrier_csf(cmdbuf, post);
      }

      return;
   }

   emit_barrier_csf(cmdbuf, deps);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPipelineBarrier2)(VkCommandBuffer commandBuffer,
                                    const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_cs_deps deps = {0};

   {
      VkPipelineStageFlags2 src = 0, dst = 0;
      for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) {
         src |= pDependencyInfo->pMemoryBarriers[i].srcStageMask;
         dst |= pDependencyInfo->pMemoryBarriers[i].dstStageMask;
      }
      for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) {
         src |= pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;
         dst |= pDependencyInfo->pImageMemoryBarriers[i].dstStageMask;
      }
      panvk_per_arch(rp_trace)("BARRIER src/dst/nimg/nbuf", src, dst,
                               pDependencyInfo->imageMemoryBarrierCount,
                               pDependencyInfo->bufferMemoryBarrierCount);
   }
   if (cmdbuf->state.gfx.rp_merge.pending &&
       !panvk_per_arch(cmd_rp_merge_barrier_ok)(cmdbuf, pDependencyInfo))
      panvk_per_arch(cmd_rp_merge_flush)(cmdbuf);

   panvk_per_arch(add_cs_deps)(cmdbuf, pDependencyInfo, &deps, false);

   if (deps.needs_fb_barrier)
      panvk_per_arch(cmd_fb_barrier)(cmdbuf);

   uint32_t max_crc_count = pDependencyInfo->imageMemoryBarrierCount;
   STACK_ARRAY(uint64_t, crc_addrs, max_crc_count);

   if (!crc_addrs) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   panvk_per_arch(collect_crc_invalidation_deps)(pDependencyInfo, &deps,
                                                 crc_addrs);

   panvk_per_arch(emit_barrier)(cmdbuf, deps);

   STACK_ARRAY_FINISH(crc_addrs);

   panvk_per_arch(cmd_bc_decode_zero_initialized)(cmdbuf, pDependencyInfo);
}

static struct cs_buffer
alloc_cs_buffer(void *cookie)
{
   struct panvk_cmd_buffer *cmdbuf = cookie;
   const unsigned capacity = 64 * 1024 / sizeof(uint64_t);

   struct pan_ptr ptr = panvk_cmd_alloc_dev_mem(cmdbuf, cs, capacity * 8, 64);

   return (struct cs_buffer){
      .cpu = ptr.cpu,
      .gpu = ptr.gpu,
      .capacity = capacity,
   };
}

static enum cs_reg_perm
cs_reg_perm(struct cs_builder *b, unsigned reg)
{
   struct panvk_cs_state *cs_state =
      container_of(b, struct panvk_cs_state, builder);
   struct panvk_cs_reg_upd_context *upd_ctx;

   for (upd_ctx = cs_state->reg_access.upd_ctx_stack; upd_ctx;
        upd_ctx = upd_ctx->next) {
      if (upd_ctx->reg_perm(b, reg) == CS_REG_RW)
         return CS_REG_RW;
   }

   return cs_state->reg_access.base_perm(b, reg);
}

static void
init_cs_builder(struct panvk_cmd_buffer *cmdbuf, uint32_t i)
{
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(cmdbuf->vk.base.device->physical);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const reg_perm_cb_t base_reg_perms[PANVK_SUBQUEUE_COUNT] = {
      [PANVK_SUBQUEUE_VERTEX_TILER] = panvk_cs_vt_reg_perm,
      [PANVK_SUBQUEUE_FRAGMENT] = panvk_cs_frag_reg_perm,
      [PANVK_SUBQUEUE_COMPUTE] = panvk_cs_compute_reg_perm,
   };

   const struct drm_panthor_csif_info *csif_info =
      panvk_get_csif_props(dev);

   {
      struct cs_builder *b = &cmdbuf->state.cs[i].builder;
      /* Lazy allocation of the root CS. */
      struct cs_buffer root_cs = {0};

      struct cs_builder_conf conf = {
         .nr_registers = csif_info->cs_reg_count,
         .nr_kernel_registers = MAX2(csif_info->unpreserved_cs_reg_count, 4),
         .compute_ep_limit =
            PAN_ARCH >= 12 ? phys_dev->kmod.dev->props.max_tasks_per_core : 0,
         .alloc_buffer = alloc_cs_buffer,
         .cookie = cmdbuf,
         .ls_sb_slot = SB_ID(LS),
      };

      if (PANVK_DEBUG(CS)) {
         cmdbuf->state.cs[i].reg_access.upd_ctx_stack = NULL;
         cmdbuf->state.cs[i].reg_access.base_perm = base_reg_perms[i];
         conf.reg_perm = cs_reg_perm;
      }

      cs_builder_init(b, &conf, root_cs);

      if (PANVK_DEBUG(TRACE)) {
         cmdbuf->state.cs[i].tracing = (struct cs_tracing_ctx){
            .enabled = true,
            .ctx_reg = cs_subqueue_ctx_reg(b),
            .tracebuf_addr_offset =
               offsetof(struct panvk_cs_subqueue_context, debug.tracebuf.cs),
         };
      }
   }
}

static void
init_cs_builders(struct panvk_cmd_buffer *cmdbuf)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)
      init_cs_builder(cmdbuf, i);
}

/* A secondary cannot split the render context it inherits. An
 * attachment-less one whose rasterizationSamples changes ends its CS segment
 * here and the primary splits the context between the segments
 * (CmdExecuteCommands). Must not be called inside a CS block.
 */
void
panvk_per_arch(cmd_cut_vmr_segment)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);

   /* The primary's context split between segments moves its sync points:
    * the next segment must not carry relative ones across it.
    */
   flush_sync_points(cmdbuf);

   struct panvk_vmr_segment *seg =
      util_dynarray_grow(&cmdbuf->vmr_segs, struct panvk_vmr_segment, 1);
   if (!seg) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   memset(seg, 0, sizeof(*seg));
   seg->samples = cmdbuf->state.gfx.render.vmr_samples;

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);

      assert(!b->blocks.stack);
      /* An empty stream just carries over to the next segment. */
      if (cs_is_empty(b))
         continue;

      /* Like finish_cs(): the primary's code after the call does not know
       * this stream's pending loads.
       */
      cs_wait_slots(b, dev->csf.sb.all_mask);
      cs_end(b);
      if (!cs_is_valid(b)) {
         vk_command_buffer_set_error(&cmdbuf->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }

      seg->addr[i] = cs_root_chunk_gpu_addr(b);
      seg->size[i] = cs_root_chunk_size(b);
      seg->req_resource_mask[i] = b->req_resource_mask;
      cs_builder_fini(b);
      init_cs_builder(cmdbuf, i);
   }

   /* The primary's context emission runs between the segments: re-emit
    * all the draw state in the next one.
    */
   vk_dynamic_graphics_state_dirty_all(&cmdbuf->vk.dynamic_graphics_state);
   gfx_state_set_all_dirty(cmdbuf);
}

static void
panvk_reset_cmdbuf(struct vk_command_buffer *vk_cmdbuf,
                   VkCommandBufferResetFlags flags)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);
   struct panvk_cmd_pool *pool =
      container_of(vk_cmdbuf->pool, struct panvk_cmd_pool, vk);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);

   vk_command_buffer_reset(&cmdbuf->vk);

   panvk_pool_reset(&cmdbuf->cs_pool);
   panvk_pool_reset(&cmdbuf->desc_pool);
   panvk_pool_reset(&cmdbuf->tls_pool);
   list_splicetail(&cmdbuf->push_sets, &pool->push_sets);
   list_inithead(&cmdbuf->push_sets);

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->utrace.uts); i++) {
      struct u_trace *ut = &cmdbuf->utrace.uts[i];
      u_trace_fini(ut);
      u_trace_init(ut, &dev->utrace.utctx);
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)
      cs_builder_fini(&cmdbuf->state.cs[i].builder);

   memset(&cmdbuf->state, 0, sizeof(cmdbuf->state));
   util_dynarray_clear(&cmdbuf->vmr_segs);
   init_cs_builders(cmdbuf);
}

static void
panvk_destroy_cmdbuf(struct vk_command_buffer *vk_cmdbuf)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);
   struct panvk_cmd_pool *pool =
      container_of(vk_cmdbuf->pool, struct panvk_cmd_pool, vk);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->utrace.uts); i++)
      u_trace_fini(&cmdbuf->utrace.uts[i]);

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)
      cs_builder_fini(&cmdbuf->state.cs[i].builder);

   util_dynarray_fini(&cmdbuf->vmr_segs);
   panvk_pool_cleanup(&cmdbuf->cs_pool);
   panvk_pool_cleanup(&cmdbuf->desc_pool);
   panvk_pool_cleanup(&cmdbuf->tls_pool);
   list_splicetail(&cmdbuf->push_sets, &pool->push_sets);
   vk_command_buffer_finish(&cmdbuf->vk);
   vk_free(&dev->vk.alloc, cmdbuf);
}

static VkResult
panvk_create_cmdbuf(struct vk_command_pool *vk_pool, VkCommandBufferLevel level,
                    struct vk_command_buffer **cmdbuf_out)
{
   struct panvk_device *device =
      container_of(vk_pool->base.device, struct panvk_device, vk);
   struct panvk_cmd_pool *pool =
      container_of(vk_pool, struct panvk_cmd_pool, vk);
   struct panvk_cmd_buffer *cmdbuf;

   cmdbuf = vk_zalloc(&device->vk.alloc, sizeof(*cmdbuf), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmdbuf)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(
      &pool->vk, &cmdbuf->vk, &panvk_per_arch(cmd_buffer_ops), level);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, cmdbuf);
      return result;
   }

   list_inithead(&cmdbuf->push_sets);
   cmdbuf->vk.dynamic_graphics_state.vi = &cmdbuf->state.gfx.dynamic.vi;
   cmdbuf->vk.dynamic_graphics_state.ms.sample_locations =
      &cmdbuf->state.gfx.dynamic.sl;

   struct panvk_pool_properties cs_pool_props = {
      .create_flags =
         panvk_device_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_WB_MMAP),
      .slab_size = 64 * 1024,
      .label = "Command buffer CS pool",
      .prealloc = false,
      .owns_bos = true,
      .needs_locking = false,
   };
   panvk_pool_init(&cmdbuf->cs_pool, device, &pool->cs_bo_pool, NULL, &cs_pool_props);

   struct panvk_pool_properties desc_pool_props = {
      .create_flags =
         panvk_device_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_WB_MMAP),
      .slab_size = 64 * 1024,
      .label = "Command buffer descriptor pool",
      .prealloc = false,
      .owns_bos = true,
      .needs_locking = false,
   };
   panvk_pool_init(&cmdbuf->desc_pool, device, &pool->desc_bo_pool, NULL,
                   &desc_pool_props);

   struct panvk_pool_properties tls_pool_props = {
      .create_flags =
         panvk_device_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_NO_MMAP),
      .slab_size = 64 * 1024,
      .label = "TLS pool",
      .prealloc = false,
      .owns_bos = true,
      .needs_locking = false,
   };
   panvk_pool_init(&cmdbuf->tls_pool, device, &pool->tls_bo_pool, &pool->tls_big_bo_pool,
                   &tls_pool_props);

   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->utrace.uts); i++)
      u_trace_init(&cmdbuf->utrace.uts[i], &device->utrace.utctx);

   init_cs_builders(cmdbuf);
   *cmdbuf_out = &cmdbuf->vk;
   return VK_SUCCESS;
}

const struct vk_command_buffer_ops panvk_per_arch(cmd_buffer_ops) = {
   .create = panvk_create_cmdbuf,
   .reset = panvk_reset_cmdbuf,
   .destroy = panvk_destroy_cmdbuf,
};

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(BeginCommandBuffer)(VkCommandBuffer commandBuffer,
                                   const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   vk_command_buffer_begin(&cmdbuf->vk, pBeginInfo);
   cmdbuf->flags = pBeginInfo->flags;

   /* Detect the engine (and log the barrier write rule) once, up front. */
   (void)panvk_per_arch(cmd_d3d12_semantics)(cmdbuf);

   if (PANVK_DEBUG(FORCE_SIMULTANEOUS)) {
      cmdbuf->flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
      cmdbuf->flags &= ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   }

   panvk_per_arch(cmd_inherit_render_state)(cmdbuf, pBeginInfo);

   if (cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
      const VkCommandBufferInheritanceConditionalRenderingInfoEXT *cond_info =
         vk_find_struct_const(
            pBeginInfo->pInheritanceInfo->pNext,
            COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT);

      if (cond_info && cond_info->conditionalRenderingEnable)
         cmdbuf->state.cond_render.inherited = true;
   }

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      panvk_per_arch(panvk_instr_begin_work)(i, cmdbuf,
                                             PANVK_INSTR_WORK_TYPE_CMDBUF);

   return VK_SUCCESS;
}

static void
panvk_cmd_invalidate_state(struct panvk_cmd_buffer *cmdbuf)
{
   /* From the Vulkan 1.3.275 spec:
    *
    *    "...There is one exception to this rule - if the primary command
    *    buffer is inside a render pass instance, then the render pass and
    *    subpass state is not disturbed by executing secondary command
    *    buffers."
    *
    * We need to reset everything EXCEPT the render pass state.
    */
   struct panvk_rendering_state render_save = cmdbuf->state.gfx.render;
   memset(&cmdbuf->state.gfx, 0, sizeof(cmdbuf->state.gfx));
   cmdbuf->state.gfx.render = render_save;

   vk_dynamic_graphics_state_dirty_all(&cmdbuf->vk.dynamic_graphics_state);
   gfx_state_set_all_dirty(cmdbuf);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdExecuteCommands)(VkCommandBuffer commandBuffer,
                                   uint32_t commandBufferCount,
                                   const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, primary, commandBuffer);
   panvk_cmd_rp_merge_flush_pending(primary);
   panvk_cmd_lazy_vt_flush_pending(primary, PANVK_LAZY_VT_EXEC);

   if (commandBufferCount == 0)
      return;

   /* Write out any pending seqno changes to registers before calling
    * secondary command buffers. */
   flush_sync_points(primary);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(panvk_cmd_buffer, secondary, pCommandBuffers[i]);

      if (UINT64_MAX - primary->state.tiler_work_estimate <
          secondary->state.tiler_work_estimate)
         primary->state.tiler_work_estimate = UINT64_MAX;
      else
         primary->state.tiler_work_estimate +=
            secondary->state.tiler_work_estimate;

      /* make sure the CS context is setup properly
       * to inherit the primary command buffer state
       */
      primary->state.tls.info.tls.size =
         MAX2(primary->state.tls.info.tls.size,
              secondary->state.tls.info.tls.size);
      const struct panvk_vmr_segment *segs = secondary->vmr_segs.data;
      const uint32_t seg_count = util_dynarray_num_elements(
         &secondary->vmr_segs, struct panvk_vmr_segment);
      panvk_per_arch(cmd_prepare_exec_cmd_for_draws)(
         primary, secondary,
         seg_count ? segs[0].samples : secondary->state.gfx.render.vmr_samples);

      /* Write the conditional rendering flag into the subqueue context
       * for inherited secondaries. The secondary's draws load from this
       * field and skip when it is zero.
       */
      if (secondary->state.cond_render.inherited) {
         for (uint32_t j = 0; j < ARRAY_SIZE(primary->state.cs); j++) {
            struct cs_builder *sec_b = panvk_get_cs_builder(secondary, j);
            if (cs_is_empty(sec_b) && !seg_count)
               continue;

            struct cs_builder *prim_b = panvk_get_cs_builder(primary, j);
            struct cs_index flag_val = cs_scratch_reg32(prim_b, 0);

            /* When the caller itself inherited, the flag is already
             * in the subqueue context from our caller.
             */
            if (primary->state.cond_render.inherited)
               continue;

            if (primary->state.cond_render.enabled) {
               /* Primary has direct conditional rendering, evaluate
                * the predicate and normalize to a 0/non-zero flag.
                */
               struct cs_index pred_addr = cs_scratch_reg64(prim_b, 2);

               cs_move64_to(prim_b, pred_addr,
                            primary->state.cond_render.addr);
               cs_load32_to(prim_b, flag_val, pred_addr, 0);

               if (primary->state.cond_render.exec_cond ==
                   MALI_CS_CONDITION_EQUAL) {
                  /* Inverted: render when pred == 0, so flip. */
                  cs_if(prim_b, MALI_CS_CONDITION_NEQUAL, flag_val)
                     cs_move32_to(prim_b, flag_val, 0);
                  cs_else(prim_b)
                     cs_move32_to(prim_b, flag_val, 1);
               }
            } else {
               /* No conditional rendering active, always render. */
               cs_move32_to(prim_b, flag_val, 1);
            }

            cs_store32(prim_b, flag_val, cs_subqueue_ctx_reg(prim_b),
                       offsetof(struct panvk_cs_subqueue_context,
                                cond_render_flag));
            cs_flush_stores(prim_b);
         }
      }

      /* Segments of an attachment-less secondary, each run in a context
       * with its sample count (cmd_cut_vmr_segment). The context work
       * moves the primary's sync points: flush them before each call.
       */
      for (uint32_t s = 0; s < seg_count; s++) {
         const struct panvk_vmr_segment *seg = segs + s;

         if (s > 0)
            panvk_per_arch(cmd_prepare_exec_cmd_for_draws)(primary, secondary,
                                                           seg->samples);
         flush_sync_points(primary);
         for (uint32_t j = 0; j < ARRAY_SIZE(primary->state.cs); j++) {
            if (!seg->size[j])
               continue;

            struct cs_builder *prim_b = panvk_get_cs_builder(primary, j);
            struct cs_index addr = cs_scratch_reg64(prim_b, 0);
            struct cs_index size = cs_scratch_reg32(prim_b, 2);
            cs_move64_to(prim_b, addr, seg->addr[j]);
            cs_move32_to(prim_b, size, seg->size[j]);
            cs_call(prim_b, addr, size);
            prim_b->req_resource_mask |= seg->req_resource_mask[j];
         }
      }
      if (seg_count)
         panvk_per_arch(cmd_prepare_exec_cmd_for_draws)(
            primary, secondary, secondary->state.gfx.render.vmr_samples);
      flush_sync_points(primary);

      for (uint32_t j = 0; j < ARRAY_SIZE(primary->state.cs); j++) {
         struct cs_builder *sec_b = panvk_get_cs_builder(secondary, j);
         assert(cs_is_valid(sec_b));
         if (!cs_is_empty(sec_b)) {
            struct cs_builder *prim_b = panvk_get_cs_builder(primary, j);
            struct cs_index addr = cs_scratch_reg64(prim_b, 0);
            struct cs_index size = cs_scratch_reg32(prim_b, 2);
            cs_move64_to(prim_b, addr, cs_root_chunk_gpu_addr(sec_b));
            cs_move32_to(prim_b, size, cs_root_chunk_size(sec_b));
            cs_call(prim_b, addr, size);
            prim_b->req_resource_mask |= sec_b->req_resource_mask;

            struct u_trace *prim_ut = &primary->utrace.uts[j];
            struct u_trace *sec_ut = &secondary->utrace.uts[j];
            u_trace_clone_append(u_trace_begin_iterator(sec_ut),
                                 u_trace_end_iterator(sec_ut), prim_ut, prim_b,
                                 panvk_per_arch(utrace_copy_buffer));
         }
      }

      /* We need to propagate the suspending state of the secondary command
       * buffer if we want to avoid poisoning the reg file when the secondary
       * command buffer suspended the render pass. */
      primary->state.gfx.render.suspended =
         secondary->state.gfx.render.suspended;

      /* Inherit the occlusion query state so that if the secondary contains
       * begin/end, but the renderpass ends in the primary, the primary can
       * correctly detect that an oq has ended in EndRendering. */
      primary->state.gfx.render.oq = secondary->state.gfx.render.oq;

      /* Same for pipeline statistics queries ended in the secondary. */
      for (uint32_t p = 0; p < secondary->state.gfx.render.pstats_pending_count;
           p++) {
         if (primary->state.gfx.render.pstats_pending_count ==
             PANVK_PSTATS_MAX_PENDING)
            panvk_per_arch(cmd_pstats_flush_pending)(primary);

         primary->state.gfx.render.pstats_pending
            [primary->state.gfx.render.pstats_pending_count++] =
            secondary->state.gfx.render.pstats_pending[p];
      }

      /* If the render context we passed to the secondary command buffer got
       * invalidated, reset the FB/tiler descs and treat things as if we
       * suspended the render pass, since those descriptors have been
       * re-emitted by the secondary command buffer already. */
      if (secondary->state.gfx.render.invalidate_inherited_ctx) {
         memset(&primary->state.gfx.render.fbds, 0,
                sizeof(primary->state.gfx.render.fbds));
         primary->state.gfx.render.tiler = 0;
         primary->state.gfx.render.flags |= VK_RENDERING_RESUMING_BIT;
      }
   }

   /* From the Vulkan 1.3.275 spec:
    *
    *    "When secondary command buffer(s) are recorded to execute on a
    *    primary command buffer, the secondary command buffer inherits no
    *    state from the primary command buffer, and all state of the primary
    *    command buffer is undefined after an execute secondary command buffer
    *    command is recorded. There is one exception to this rule - if the
    *    primary command buffer is inside a render pass instance, then the
    *    render pass and subpass state is not disturbed by executing secondary
    *    command buffers. For state dependent commands (such as draws and
    *    dispatches), any state consumed by those commands must not be
    *    undefined."
    *
    * Therefore, it's the client's job to reset all the state in the primary
    * after the secondary executes.  However, if we're doing any internal
    * dirty tracking, we may miss the fact that a secondary has messed with
    * GPU state if we don't invalidate all our internal tracking.
    */
   panvk_cmd_invalidate_state(primary);
}

static bool
image_barrier_invalidates_crc(const VkImageMemoryBarrier2 *barrier,
                              const struct panvk_image *image)
{
   if (barrier->oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED)
      return true;

   const bool external_acquire =
      barrier->srcQueueFamilyIndex != barrier->dstQueueFamilyIndex &&
      (barrier->srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL ||
       barrier->srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);

   if (!external_acquire)
      return false;

   if (image->crc_safe_external)
      return false;

   const VkExternalMemoryAcquireUnmodifiedEXT *unmodified =
      vk_find_struct_const(barrier->pNext,
                           EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT);

   return !unmodified || !unmodified->acquireUnmodifiedMemory;
}

static uint64_t
panvk_per_arch(image_barrier_crc_header_addr)(
   const VkImageMemoryBarrier2 *barrier)
{
   const VkImageSubresourceRange *range = &barrier->subresourceRange;
   if (range->baseMipLevel != 0)
      return 0;

   if (!(range->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
      return 0;

   VK_FROM_HANDLE(panvk_image, image, barrier->image);

   if (!image_barrier_invalidates_crc(barrier, image))
      return 0;

   return panvk_image_plane_crc_header_addr(image);
}

void
panvk_per_arch(collect_crc_invalidation_deps)(const VkDependencyInfo *info,
                                              struct panvk_cs_deps *deps,
                                              uint64_t *crc_addrs)
{
   assert(!deps->crc.addrs);
   assert(!deps->crc.count);

   deps->crc.addrs = crc_addrs;

   for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *barrier = &info->pImageMemoryBarriers[i];
      uint64_t addr = panvk_per_arch(image_barrier_crc_header_addr)(barrier);

      if (!addr)
         continue;

      bool duplicate = false;
      for (uint32_t j = 0; j < deps->crc.count; j++) {
         duplicate |= crc_addrs[j] == addr;
      }

      if (!duplicate) {
         crc_addrs[deps->crc.count++] = addr;
      }

      struct panvk_sync_scope src = {
         barrier->srcStageMask,
         barrier->srcAccessMask,
      };
      struct panvk_sync_scope dst = {
         barrier->dstStageMask,
         barrier->dstAccessMask,
      };

      normalize_dependency(&src, &dst, barrier->srcQueueFamilyIndex,
                           barrier->dstQueueFamilyIndex);

      deps->crc.src_subqueue_mask |=
         vk_stages_to_subqueue_mask(src.stages, SYNC_SCOPE_FIRST);
      deps->crc.dst_subqueue_mask |=
         vk_stages_to_subqueue_mask(dst.stages, SYNC_SCOPE_SECOND);
   }
}

void
panvk_per_arch(cmd_invalidate_crc)(struct cs_builder *b,
                                   uint64_t crc_header_addr)
{
   if (!crc_header_addr)
      return;

#if PAN_ARCH == 10
   struct cs_index addr = cs_scratch_reg64(b, 0);
   struct cs_index valid = cs_scratch_reg32(b, 2);

   cs_move64_to(b, addr, crc_header_addr);
   cs_move32_to(b, valid, 0);

   cs_store32(b, valid, addr, PAN_CRC_VALID_OFFSET);
#elif PAN_ARCH >= 11
   struct cs_index addr = cs_scratch_reg64(b, 0);
   struct cs_index init = cs_scratch_reg32(b, 2);
   struct cs_index mask = cs_scratch_reg32(b, 3);

   cs_move64_to(b, addr, crc_header_addr);
   cs_load32_to(b, init, addr, PAN_CRC_INIT_OFFSET);

   cs_add_imm32(b, init, init, 1);

   cs_move32_to(b, mask, PAN_CRC_INIT_MASK);
   cs_and32(b, init, init, mask);

   cs_store32(b, init, addr, PAN_CRC_INIT_OFFSET);
#endif

   cs_flush_stores(b);
}
