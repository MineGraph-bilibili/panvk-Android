/* Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "kraid.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/model/pan_model.h"

enum bi_va_lod_mode {
    BI_VA_LOD_MODE_ZERO_LOD = 0,
    BI_VA_LOD_MODE_COMPUTED_LOD = 1,
    BI_VA_LOD_MODE_EXPLICIT = 2,
    BI_VA_LOD_MODE_COMPUTED_BIAS = 3,
    BI_VA_LOD_MODE_GRDESC = 4,
};

static inline uint64_t val_ex_fifo_varying_bits() {
   return VALHAL_EX_FIFO_VARYING_BITS;
}

static inline const char *
kraid_shader_stage_name(mesa_shader_stage stage, bool is_blend,
                        enum kraid_idvs_mode idvs)
{
   if (idvs == KRAID_IDVS_VARYING)
      return "MESA_SHADER_VARYING";
   else if (idvs == KRAID_IDVS_POSITION)
      return "MESA_SHADER_POSITION";
   else if (is_blend)
      return "MESA_SHADER_BLEND";
   else
      return mesa_shader_stage_name(stage);
}
