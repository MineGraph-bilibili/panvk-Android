// Copyright © 2026 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::builder::*;
use crate::ir::*;
use crate::ops::*;

fn legalize_op_swizzles(b: &mut impl SSABuilder, op: &mut Op) {
    let mut swizzle_bits = [0_u8; Instr::MAX_SRC_COUNT];
    debug_assert!(op.srcs().len() <= swizzle_bits.len());

    for (i, (src, src_type)) in op.srcs_types().enumerate() {
        if b.model().op_src_supports_swizzle(op, src, src.swizzle) {
            continue;
        }

        swizzle_bits[i] = if src.replicates_byte()
            && b.model().op_src_supports_swizzle(op, src, Swizzle::B0000)
        {
            8
        } else if src.replicates_half()
            && b.model().op_src_supports_swizzle(op, src, Swizzle::H00)
        {
            16
        } else {
            assert!(b.model().op_src_supports_swizzle(op, src, Swizzle::NONE));
            src_type.total_bits()
        };
    }

    for (i, (op_src, src_type)) in op.srcs_types_mut().enumerate() {
        if swizzle_bits[i] == 0 {
            continue;
        }

        let swz_type = DataType::get(
            swizzle_bits[i] / src_type.bits(),
            src_type.num_type(),
            src_type.bits(),
        );

        let tmp = b.alloc_ssa(swizzle_bits[i]);
        let mut src = Src::from(tmp);
        std::mem::swap(&mut op_src.src_ref, &mut src.src_ref);
        std::mem::swap(&mut op_src.swizzle, &mut src.swizzle);
        b.push_op(OpSwz {
            dst: tmp.into(),
            src_type: swz_type,
            src,
        });
    }
}

impl Shader<'_> {
    /// This pass gets run after ALU op widening and ensures that we only use
    /// valid swizzles.  This lets us be a bit sloppy in NIR -> Kraid and just
    /// assume every V4I8 source can do full swizzles and any V2X16 source can
    /// do half swizzles.  After this pass is run, it's expected that Kraid will
    /// keep swizzles valid going forwards.
    ///
    /// There are a few ops this pass cannot fix such as IMUL.[su]64 because it
    /// doesn't support the NONE swizzle.  Instead, it's up to NIR -> Kraid to
    /// get the swizzle right and this pass will assert fail if we screw it up.
    pub fn legalize_src_swizzles(&mut self) {
        let model = self.model;
        self.map_instrs(|mut instr, ssa_alloc| {
            let mut b = SSAInstrBuilder::new(model, ssa_alloc);
            legalize_op_swizzles(&mut b, &mut instr.op);
            b.push_instr(instr);
            b.into_mapped()
        });
        self.info.src_swizzles_legal = true;
    }
}
