/*
 * Copyright © 2026 panvk-kbase-android contributors
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "panvk_bc_eac_lut.h"
#include "panvk_image.h"

const uint32_t *
panvk_bc_eac_lut_data(void)
{
   return panvk_bc_eac_lut;
}

size_t
panvk_bc_eac_lut_size(void)
{
   return sizeof(panvk_bc_eac_lut);
}
