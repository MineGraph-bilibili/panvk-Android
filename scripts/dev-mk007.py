#!/usr/bin/env python3
"""Assemble patch 007: CSF gpu_queue kbase submit path (filtered hunks)."""
import re, subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'

def udiff(a, b):
    p = subprocess.run(['diff', '-U3', a, b], capture_output=True, text=True)
    assert p.returncode in (0, 1), p.stderr
    return p.stdout

def kept_hunks(diff_text, pattern='kbase'):
    lines = diff_text.splitlines(keepends=True)
    body = ''.join(l for l in lines if not l.startswith(('--- ', '+++ ')))
    hunks = [h for h in re.split(r'(?=@@ )', body) if h.strip()]
    return [h for h in hunks if re.search(pattern, h, re.I)]

d = udiff(f'{M}/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c',
          f'{B}/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c')
k = kept_hunks(d)
assert k, 'no hunks'
parts = ['--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c\n',
         '+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c\n'] + k
header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH csf 007] panvk: kbase CSF queue submit path

Purpose: Route CSF queue create/destroy/submit through the kbase backend
 (userspace CS rings, CSG/group lifecycle, seqno polling, tiler-heap
 renewal, dmabuf/sync-file interop) when the device came from /dev/mali*;
 DRM/panthor path unchanged otherwise.
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4
 (src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c), kbase-containing hunks.
 Removals are DRM->kbase branch replacements at the same call sites
 (create_group, init_tiler, syncobj wait/destroy, tiler-heap ioctls).
Tested GPU: none yet (compile-level).
Tested Kbase UAPI: n/a.
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: 003/004/006 (kbase state, sync helpers, progress markers).
Validation test: x86 smoke build; runtime Gates C-H on Poco.

"""
open('patches/csf/007-gpu-queue-kbase.patch', 'w').write(header + ''.join(parts))
print('wrote 007, hunks:', len(k))
