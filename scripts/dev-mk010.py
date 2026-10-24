#!/usr/bin/env python3
"""Assemble patch 010: use pan_kmod_bo_munmap() for BO mappings.

On kbase SAME_VA the CPU mapping belongs to the BO; raw os_munmap()
destroys the GPU mapping too (second Map returns a dead address ->
SEGV on write; GPU hangs). Beta routes all BO unmaps through the
backend hook (no-op on kbase). These hunks contain 'munmap' but no
'kbase', so the earlier filters missed them.
"""
import re, subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'

def udiff(a, b):
    p = subprocess.run(['diff', '-U3', a, b], capture_output=True, text=True)
    assert p.returncode in (0, 1), p.stderr
    return p.stdout

def kept_hunks(diff_text, pattern):
    lines = diff_text.splitlines(keepends=True)
    body = ''.join(l for l in lines if not l.startswith(('--- ', '+++ ')))
    hunks = [h for h in re.split(r'(?=@@ )', body) if h.strip()]
    return [h for h in hunks if re.search(pattern, h)]

FILES = [
    'src/panfrost/vulkan/panvk_device_memory.c',
    'src/panfrost/vulkan/panvk_host_copy.c',
    'src/panfrost/vulkan/panvk_image.c',
    'src/panfrost/vulkan/panvk_priv_bo.c',
    'src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c',
]
parts = []
for f in FILES:
    d = udiff(f'{M}/{f}', f'{B}/{f}')
    k = kept_hunks(d, r'pan_kmod_bo_munmap')
    # gpu_queue hunks must be taken only if they apply on top of 007
    parts.append((f, k))

header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH kbase-common 010] panvk: unmap BOs via backend hook

Purpose: Route every BO CPU-unmap through pan_kmod_bo_munmap() instead of
 raw os_munmap(). On kbase SAME_VA the mapping is owned by the BO and
 lives until bo_free; raw munmap destroys the GPU mapping too, so the
 next Map returns a dead address (SEGV on CPU write, GPU hangs). Fixes
 the Gate E multi-iteration crash (iter 0 fence-wait passed, iter 1
 write faulted at the same VA).
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4 (munmap call sites only).
Tested GPU: Poco X6 Pro / G615 (fixes Gate E crash).
Tested Kbase UAPI: CSF 1.21.
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: 003 (hook), 007 (gpu_queue context).
Validation test: Gate E compute 10/10 with map/unmap per iteration.

"""
body = []
for f, k in parts:
    body.append(f'--- a/{f}\n')
    body.append(f'+++ b/{f}\n')
    body += k
open('patches/kbase-common/010-bo-munmap-hook.patch', 'w').write(
    header + ''.join(body))
print('wrote 010, files:', [(f, len(k)) for f, k in parts])
