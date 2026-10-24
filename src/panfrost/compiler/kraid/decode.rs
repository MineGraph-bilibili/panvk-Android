// Copyright (c) 2026 Arm Ltd.
// SPDX-License-Identifier: MIT

use crate::isa::v9;

pub use v9::decode::{Mnemonic, PrintCtx, Variant, print, try_decode};

#[cfg(test)]
mod tests {
    use crate::isa::InvalidInstrError;

    use super::*;
    use paste::paste;

    fn decode_to_string(
        val: u64,
        arch: u8,
        ctx: PrintCtx,
    ) -> Result<String, InvalidInstrError> {
        let mut buffer = Vec::new();

        match try_decode(val, arch) {
            Ok((mn, var)) => {
                print(val, mn, var, arch, &mut buffer, &ctx).unwrap();
                let result_string = String::from_utf8(buffer).unwrap();
                Ok(result_string)
            }
            Err(err) => Err(err),
        }
    }

    macro_rules! print_ctx {
        ($( $field:ident = $value:expr ),*) => {{
            let mut ctx: PrintCtx = Default::default();

            $(ctx.$field = $value;);*

            ctx
        }};
    }

    macro_rules! disasm_case {
        ($val:tt, $result:literal, $ctx:expr) => {
            paste! {
                #[test]
                fn [<decode_ $val>]() {
                    let res = decode_to_string($val, 13u8, $ctx);
                    assert!(res.is_ok());
                    assert_eq!(res.unwrap().as_str(), $result);
                }
            }
        };
        ($val:tt, $result:literal) => {
            disasm_case!($val, $result, Default::default());
        };
    }

    disasm_case!(0x00a4c8d00000c048, "FADD.f32 r8, r8^.abs.neg, k0.neg");

    disasm_case!(
        0x00728a46303b903c,
        "LD_CHECKED_IMM.i96.slot0 @r10:r11:r12, [r60:r61], #0x490, r59"
    );

    disasm_case!(
        0x001fc00000001842,
        "BRANCH r2^, 0xbfb7",
        print_ctx!(pc_base = 0xbeef)
    );

    disasm_case!(0x001fc007fffffe5b, "BRANCH r27^, 0xfffffffffffffff8");

    disasm_case!(
        0x007f40083230fe0a,
        "BLEND.slot0.v4.f32 @r0:r1:r2:r3, [r10:r11], 0x7f8, r48"
    );

    disasm_case!(
        0x007f40043330fe0a,
        "BLEND.slot0.v4.f16 @r0:r1, [r10:r11], 0x7f8, r48"
    );

    disasm_case!(
        0x0920c0041fc70048,
        "ACMPXCHG.i32.slot0.wait0 @r7, @r0:r1, [r8^:r9^], #0x0"
    );

    disasm_case!(0x00b4c9015091c014, "AND.i32.not r9, r20, u8.w1");
    disasm_case!(
        0x00b4c9015091c015,
        "AND.i32.not r9, r21, u17",
        print_ctx!(fau32 = true)
    );

    // This one is ambiguous, the bit pattern matches FMUL_RSCALE as well.
    // It should be decoded as FADD_RSCALE.
    disasm_case!(0x0160c00404c0d001, "FADD_RSCALE.f32 r0, r1, k0.neg, r4");
    // If src1 is not #1.0, then it should become an FMUL_RSCALE.
    disasm_case!(0x0160c00404c00201, "FMUL_RSCALE.f32 r0, r1, r2, r4");
    // And if src2 is not 0 either, it should become FMA_RSCALE.
    disasm_case!(0x0160c00404030201, "FMA_RSCALE.f32 r0, r1, r2, r3.neg, r4");
}
