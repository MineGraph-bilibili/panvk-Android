#!/bin/sh
# validate-binary.sh — Gate A static checks (ELF64/AArch64, DT_NEEDED, ICD exports)
# Usage: ./scripts/validate-binary.sh --abi android|glibc <file.so>
set -eu
ABI=""; SO=""
while [ $# -gt 0 ]; do case "$1" in --abi) ABI="$2"; shift 2;; *) SO="$1"; shift;; esac; done
[ -n "$SO" ] && [ -f "$SO" ] || { echo "file missing: $SO" >&2; exit 1; }
echo "== file =="; file "$SO"
echo "== readelf -h =="; readelf -h "$SO" | head -n 8
echo "== DT_NEEDED =="; readelf -d "$SO" | grep -i needed || true
echo "== ICD exports =="; NM="llvm-nm"; command -v llvm-nm >/dev/null || NM=nm
$NM -D "$SO" 2>/dev/null | grep -E "vk_icdGetInstanceProcAddr|vk_icdNegotiateLoaderICDInterfaceVersion|hwvulkan_module" || true
FAIL=0
if [ "$ABI" = "android" ]; then
  readelf -d "$SO" | grep -q "libc.so.6" && { echo "FAIL: android .so depends on glibc libc.so.6" >&2; FAIL=1; }
  $NM -D "$SO" 2>/dev/null | grep -q "vk_icdGetInstanceProcAddr" || { echo "FAIL: missing vk_icdGetInstanceProcAddr" >&2; FAIL=1; }
  $NM -D "$SO" 2>/dev/null | grep -q "vk_icdNegotiateLoaderICDInterfaceVersion" || { echo "FAIL: missing vk_icdNegotiateLoaderICDInterfaceVersion" >&2; FAIL=1; }
  readelf -d "$SO" | grep -qi "libX11" && { echo "FAIL: android .so depends on X11" >&2; FAIL=1; }
fi
if [ "$ABI" = "glibc" ]; then
  readelf -d "$SO" | grep -q "libc.so.6" || { echo "FAIL: glibc .so missing libc.so.6" >&2; FAIL=1; }
fi
[ "$FAIL" = 0 ] && echo "STATIC-VALIDATION: PASS ($ABI)" || { echo "STATIC-VALIDATION: FAIL ($ABI)" >&2; exit 1; }
