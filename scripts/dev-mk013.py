#!/usr/bin/env python3
"""Assemble patch 013: vendor mapper metadata (pure addition, no removals)."""
import subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'
F = 'src/util/u_gralloc/u_gralloc_fallback.c'

p = subprocess.run(['diff', '-U3', f'{M}/{F}', f'{B}/{F}'],
                   capture_output=True, text=True)
assert p.returncode == 1
d = p.stdout.splitlines(keepends=True)
d = [('--- a/' + F + '\n') if l.startswith('--- ')
     else (('+++ b/' + F + '\n') if l.startswith('+++ ') else l) for l in d]
header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH android 013] gralloc: vendor mapper metadata, refuse guesses

Purpose: Query real gralloc metadata (fourcc, DRM modifier, allocation
 size, per-plane strides/offsets/fds) through the device mapper HAL
 (AIDL mapper v5 getStandardMetadata) and REFUSE to continue when
 complete metadata is unavailable. The pinned fallback sets
 DRM_FORMAT_MOD_INVALID, which leaves PanVK with a NULL modifier
 handler and crashes in pan_image_layout_init. Plan rule: never map a
 missing modifier to LINEAR, never guess.
 Mechanism is vendor-neutral (standard mapper API); only the HAL lookup
 names are MediaTek-flavored (binder passthrough "mapper"/"mediatek",
 SPHAL "mapper.mediatek.so"), matching this target (duchamp ships
 mapper.mediatek.so). Override via PANVK_MAPPER_NAMES if ever needed
 (future work; names currently fixed).
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4
 (src/util/u_gralloc/u_gralloc_fallback.c), taken whole: pure addition,
 zero removed lines.
Tested GPU: Poco X6 Pro / G615 (Gate G needs it).
Tested Kbase UAPI: n/a (gralloc path).
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: none.
Validation test: Gate G (AHB import shows real modifier, no crash).

"""
open('patches/android/013-vendor-mapper-metadata.patch', 'w').write(
    header + ''.join(d))
print('wrote 013')
