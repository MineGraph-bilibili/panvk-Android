/*
 * Copyright (C) 2025 Arm Ltd.
 * Copyright (C) 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "bi_builder.h"
#include "va_compiler.h"
#include "valhall.h"

/* Only some special immediates are available, as specified in the Table of
 * Immediates in the specification. Other immediates must be lowered, either to
 * uniforms or to moves.
 */

static bi_index
va_mov_imm(bi_builder *b, uint32_t imm)
{
   bi_index zero = bi_fau(BIR_FAU_IMMEDIATE | 0, false);
   return bi_iadd_imm_i32(b, zero, imm);
}

static bi_index
va_lut_index_32(uint32_t imm)
{
   for (unsigned i = 0; i < ARRAY_SIZE(valhall_immediates); ++i) {
      if (valhall_immediates[i] == imm)
         return va_lut(i);
   }

   return bi_null();
}

static bi_index
va_lut_index_16(uint16_t imm)
{
   uint16_t *arr16 = (uint16_t *)valhall_immediates;

   for (unsigned i = 0; i < (2 * ARRAY_SIZE(valhall_immediates)); ++i) {
      if (arr16[i] == imm)
         return bi_half(va_lut(i >> 1), i & 1);
   }

   return bi_null();
}

UNUSED static bi_index
va_lut_index_8(uint8_t imm)
{
   uint8_t *arr8 = (uint8_t *)valhall_immediates;

   for (unsigned i = 0; i < (4 * ARRAY_SIZE(valhall_immediates)); ++i) {
      if (arr8[i] == imm)
         return bi_byte(va_lut(i >> 2), i & 3);
   }

   return bi_null();
}

static bi_index
va_demote_constant_fp16(uint32_t value)
{
   uint16_t fp16 = _mesa_float_to_half(uif(value));

   /* Only convert if it is exact */
   if (fui(_mesa_half_to_float(fp16)) == value)
      return va_lut_index_16(fp16);
   else
      return bi_null();
}

/*
 * Test if a 32-bit word arises as a sign or zero extension of some 8/16-bit
 * value.
 */
static bool
is_extension_of_8(uint32_t x, bool is_signed)
{
   if (is_signed)
      return (x <= INT8_MAX) || ((x >> 7) == BITFIELD_MASK(24 + 1));
   else
      return (x <= UINT8_MAX);
}

static bool
is_extension_of_16(uint32_t x, bool is_signed)
{
   if (is_signed)
      return (x <= INT16_MAX) || ((x >> 15) == BITFIELD_MASK(16 + 1));
   else
      return (x <= UINT16_MAX);
}

static bi_index
va_move_const_to_fau(bi_builder *b, uint32_t value)
{
   struct pan_fau_layout *fau = b->shader->info.fau;

   int idx = pan_lookup_pushed_imm(fau, value);
   if (idx >= 0) {
      return bi_fau((enum bir_fau)(BIR_FAU_UNIFORM | (idx >> 1)), idx & 1);
   }

   if (pan_fau_available(fau) > 0) {
      const unsigned int idx = pan_fau_emit_const(fau, value);
      return bi_fau((enum bir_fau)(BIR_FAU_UNIFORM | (idx >> 1)), idx & 1);
   }

   return bi_null();
}

static bi_index
va_lookup_constant(uint32_t value, struct va_src_info info, bool is_signed)
{
   /* Try the constant as-is */
   {
      bi_index lut = va_lut_index_32(value);
      if (!bi_is_null(lut))
         return lut;

      /* ...or negated as a FP32 constant */
      if (info.absneg && info.size == VA_SIZE_32) {
         lut = bi_neg(va_lut_index_32(fui(-uif(value))));
         if (!bi_is_null(lut))
            return lut;
      }

      /* ...or negated as a FP16 constant */
      if (info.absneg && info.size == VA_SIZE_16) {
         lut = bi_neg(va_lut_index_32(value ^ 0x80008000));
         if (!bi_is_null(lut))
            return lut;
      }
   }

   /* Try using a single half of a FP16 constant */
   bool replicated_halves = (value & 0xFFFF) == (value >> 16);
   if (info.swizzle && info.size == VA_SIZE_16 && replicated_halves) {
      bi_index lut = va_lut_index_16(value & 0xFFFF);
      if (!bi_is_null(lut))
         return lut;

      /* ...possibly negated */
      if (info.absneg) {
         lut = bi_neg(va_lut_index_16((value & 0xFFFF) ^ 0x8000));
         if (!bi_is_null(lut))
            return lut;
      }
   }

   /* The widening swizzle returned below selects one sub-word of a LUT entry.
    * On a 32-bit instruction the hardware zero/sign-extends that sub-word into
    * the full 32-bit lane. The same encoded swizzle has a different meaning on
    * an 8/16-bit vector instruction: it replicates the sub-word across all
    * lanes (for example .h0 is BI_SWIZZLE_H00, half 0 broadcast to both lanes),
    * so a constant like (0x3f80, 0x0000) would turn into (0x3f80, 0x3f80).
    * Only widen for 32-bit instructions. The replicated-halves case for 16-bit
    * is handled separately via info.swizzle above.
    */
   bool can_widen = info.widen && info.size == VA_SIZE_32;

   /* Try extending a byte */
   if ((can_widen || info.lanes || info.lane) &&
       is_extension_of_8(value, is_signed)) {

      bi_index lut = va_lut_index_8(value & 0xFF);
      if (!bi_is_null(lut))
         return lut;
   }

   /* Try extending a halfword */
   if (can_widen && is_extension_of_16(value, is_signed)) {

      bi_index lut = va_lut_index_16(value & 0xFFFF);
      if (!bi_is_null(lut))
         return lut;
   }

   /* Try demoting the constant to FP16 */
   if (info.swizzle && info.size == VA_SIZE_32) {
      bi_index lut = va_demote_constant_fp16(value);
      if (!bi_is_null(lut))
         return lut;

      if (info.absneg) {
         bi_index lut = bi_neg(va_demote_constant_fp16(fui(-uif(value))));
         if (!bi_is_null(lut))
            return lut;
      }
   }

   return bi_null();
}

static bi_index
va_resolve_constant(bi_builder *b, uint32_t value, struct va_src_info info,
                    bool is_signed, bool staging, bool try_move_to_fau)
{
   if (!staging) {
      bi_index lut = va_lookup_constant(value, info, is_signed);
      if (!bi_is_null(lut))
         return lut;
   }

   if (!staging && try_move_to_fau) {
      bi_index c = va_move_const_to_fau(b, value);
      if (!bi_is_null(c))
         return c;
   }

   return va_mov_imm(b, value);
}

static uint32_t
va_resolve_swizzles(bi_context *ctx, bi_instr *I, unsigned s)
{
   struct va_src_info info = va_src_info(I->op, s);
   uint32_t value = I->src[s].value;
   enum bi_swizzle swz = I->src[s].swizzle;

   /* Resolve any swizzle, keeping in mind the different interpretations
    * swizzles in different contexts.
    */
   if (info.size == VA_SIZE_32) {
      /* Extracting a half from the 32-bit value */
      if (swz == BI_SWIZZLE_H00)
         value = (value & 0xFFFF);
      else if (swz == BI_SWIZZLE_H11)
         value = (value >> 16);
      else
         assert(swz == BI_SWIZZLE_H01);

      /* FP16 -> FP32 */
      if (info.swizzle && swz != BI_SWIZZLE_H01)
         value = fui(_mesa_half_to_float(value));
   } else if (info.size == VA_SIZE_16) {
      assert(swz >= BI_SWIZZLE_H00 && swz <= BI_SWIZZLE_H11);
      value = bi_apply_swizzle(value, swz);
   } else if (info.size == VA_SIZE_8 && (info.lane || info.lanes)) {
      /* 8-bit extract */
      unsigned chan = (swz - BI_SWIZZLE_B0000);
      assert(chan < 4);

      value = (value >> (8 * chan)) & 0xFF;
   } else {
      /* TODO: Any other special handling? */
      value = bi_apply_swizzle(value, swz);
   }

   return value;
}

/*
 * A 64-bit operand read as two 32-bit sources (the texture handle of TEX, for
 * one) must come from one register pair or one 64-bit FAU slot. Two constants
 * lowered one word at a time land in different slots, and va_lower_split_64bit
 * then moves both into registers. Push the pair into one slot instead.
 */
static bool
va_lower_constant_pair(bi_context *ctx, bi_instr *I, unsigned s)
{
   struct pan_fau_layout *fau = ctx->info.fau;

   if (s + 1 >= I->nr_srcs || I->src[s].type != BI_INDEX_CONSTANT ||
       I->src[s + 1].type != BI_INDEX_CONSTANT || I->src[s].neg ||
       I->src[s + 1].neg || s < valhall_opcodes[I->op].nr_staging_srcs ||
       va_src_info(I->op, s).size != VA_SIZE_64 ||
       bi_count_read_registers(I, s) != 1)
      return false;

   const uint32_t lo = va_resolve_swizzles(ctx, I, s);
   const uint32_t hi = va_resolve_swizzles(ctx, I, s + 1);
   int idx = -1;

   for (unsigned i = ALIGN_POT(fau->reserved, 2); i + 1 < fau->count; i += 2) {
      if (BITSET_TEST(fau->is_const, i) && BITSET_TEST(fau->is_const, i + 1) &&
          fau->words[i].constant == lo && fau->words[i + 1].constant == hi) {
         idx = i;
         break;
      }
   }

   if (idx < 0) {
      if (pan_fau_available(fau) < 2 + (fau->count & 1))
         return false;
      if (fau->count & 1)
         pan_fau_emit_const(fau, 0);
      idx = pan_fau_emit_const(fau, lo);
      pan_fau_emit_const(fau, hi);
   }

   I->src[s] = bi_fau((enum bir_fau)(BIR_FAU_UNIFORM | (idx >> 1)), false);
   I->src[s + 1] = bi_fau((enum bir_fau)(BIR_FAU_UNIFORM | (idx >> 1)), true);
   return true;
}

/*
 * An instruction reads at most one 64-bit uniform slot. Two 32-bit constants
 * that no LUT entry covers (FMA x, 0.75, 0.05) used to land in different slots
 * or stay immediates, and one of them was moved into a register for every
 * thread. Put both words in one slot instead, shared with any other
 * instruction that needs the same two.
 */
static void
va_lower_two_constants(bi_context *ctx, bi_instr *I)
{
   struct pan_fau_layout *fau = ctx->info.fau;
   const bool is_signed = valhall_opcodes[I->op].is_signed;
   unsigned srcs[3], nr = 0;
   uint32_t vals[2];
   unsigned nr_vals = 0;

   bi_foreach_src(I, s) {
      bi_index src = I->src[s];

      /* Anything else in the FAU (a push constant, a special value) owns the
       * one slot this instruction may read. */
      if (src.type == BI_INDEX_FAU)
         return;
      if (src.type != BI_INDEX_CONSTANT ||
          s < valhall_opcodes[I->op].nr_staging_srcs)
         continue;

      struct va_src_info info = va_src_info(I->op, s);
      if (info.size != VA_SIZE_32 || src.swizzle != BI_SWIZZLE_H01 || src.abs)
         return;

      const uint32_t value = va_resolve_swizzles(ctx, I, s);

      /* A LUT immediate takes a word of the 64-bit budget too. */
      if (!bi_is_null(va_lookup_constant(value, info, is_signed)))
         return;

      if (!(nr_vals == 1 && vals[0] == value) &&
          !(nr_vals == 2 && (vals[0] == value || vals[1] == value))) {
         if (nr_vals == 2)
            return;
         vals[nr_vals++] = value;
      }
      srcs[nr++] = s;
      if (nr == ARRAY_SIZE(srcs))
         return;
   }

   if (nr_vals != 2)
      return;

   int idx = -1;
   for (unsigned i = ALIGN_POT(fau->reserved, 2); i + 1 < fau->count; i += 2) {
      if (!BITSET_TEST(fau->is_const, i) || !BITSET_TEST(fau->is_const, i + 1))
         continue;
      const uint32_t lo = fau->words[i].constant, hi = fau->words[i + 1].constant;
      if ((lo == vals[0] && hi == vals[1]) || (lo == vals[1] && hi == vals[0])) {
         idx = i;
         break;
      }
   }

   if (idx < 0) {
      if (pan_fau_available(fau) < 2 + (fau->count & 1))
         return;
      if (fau->count & 1)
         pan_fau_emit_const(fau, 0);
      idx = pan_fau_emit_const(fau, vals[0]);
      pan_fau_emit_const(fau, vals[1]);
   }

   for (unsigned i = 0; i < nr; i++) {
      const unsigned s = srcs[i];
      const uint32_t value = va_resolve_swizzles(ctx, I, s);
      const bool hi = fau->words[idx + 1].constant == value &&
                      fau->words[idx].constant != value;
      bi_index cons = bi_fau((enum bir_fau)(BIR_FAU_UNIFORM | (idx >> 1)), hi);
      cons.neg = I->src[s].neg;
      I->src[s] = cons;
   }
}

void
va_lower_constants(bi_context *ctx, bi_instr *I, struct hash_table_u64 *counts, uint32_t min_fau_count)
{
   bi_builder b = bi_init_builder(ctx, bi_before_instr(I));

   if (ctx->inputs->fau.promote_immediates) {
      bi_foreach_src(I, s) {
         if (va_lower_constant_pair(ctx, I, s))
            s++;
      }
      va_lower_two_constants(ctx, I);
   }

   bi_foreach_src(I, s) {
      if (I->src[s].type == BI_INDEX_CONSTANT) {
         /* abs(#c) is pointless, but -#c occurs in transcendental sequences */
         assert(!I->src[s].abs && "redundant .abs modifier");

         bool is_signed = valhall_opcodes[I->op].is_signed;
         bool staging = (s < valhall_opcodes[I->op].nr_staging_srcs);
         struct va_src_info info = va_src_info(I->op, s);
         const uint32_t value = va_resolve_swizzles(ctx, I, s);

         const uint32_t count = (uintptr_t)_mesa_hash_table_u64_search(counts, value);
         const bool move_to_fau = count >= min_fau_count;

         bi_index cons =
            va_resolve_constant(&b, value, info, is_signed, staging, move_to_fau);
         cons.neg ^= I->src[s].neg;
         I->src[s] = cons;

         /* If we're selecting a single lane, we should return a single lane
          * to ensure the result is encodeable. By convention, applying the
          * lane select puts the desired constant (at least) in the bottom
          * byte/half, so we can always select the bottom byte/half.
          */
         if ((info.lane || info.lanes || info.halfswizzle) &&
             I->src[s].swizzle == BI_SWIZZLE_H01) {
            assert(info.size == VA_SIZE_8 || info.size == VA_SIZE_16);
            if (info.size == VA_SIZE_8)
               I->src[s] = bi_byte(I->src[s], 0);
            else if (info.size == VA_SIZE_16)
               I->src[s] = bi_half(I->src[s], false);
         }
      }
   }
}

void
va_count_constants(bi_context *ctx, bi_instr *I, struct hash_table_u64 *counts)
{
   bi_foreach_src(I, s) {
      if (I->src[s].type != BI_INDEX_CONSTANT)
         continue;

      const bool staging = (s < valhall_opcodes[I->op].nr_staging_srcs);
      if (staging)
         continue;

      bool is_signed = valhall_opcodes[I->op].is_signed;
      struct va_src_info info = va_src_info(I->op, s);
      uint32_t value = va_resolve_swizzles(ctx, I, s);

      bi_index cons = va_lookup_constant(value, info, is_signed);

      const bool can_lut = !bi_is_null(cons);

      /* We want to move constants that can't be created from built-in
       * constants into the FAU if they are not staging register sources. */
      if (!can_lut) {
         uint32_t count = (uintptr_t)_mesa_hash_table_u64_search(counts, value);
         _mesa_hash_table_u64_insert(counts, value, (void*)(uintptr_t)(count + 1));
      }
   }
}
