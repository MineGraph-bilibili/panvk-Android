#!/usr/bin/env python3
"""Assemble patch 009: route remaining csif/flush-id call sites through the
kbase-aware helpers (sites whose hunks contain no 'kbase' string, missed by
the 006 filter). One-line substitutions only; the utrace clone-allocator
refactor drift is NOT taken."""
import subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'

SUBS = [
    ('src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c',
     'panthor_kmod_get_csif_props(dev->kmod.dev)',
     'panvk_get_csif_props(dev)', 1),
    ('src/panfrost/vulkan/csf/panvk_vX_exception_handler.c',
     'panthor_kmod_get_csif_props(dev->kmod.dev)',
     'panvk_get_csif_props(dev)', 1),
    ('src/panfrost/vulkan/csf/panvk_vX_utrace.c',
     'panthor_kmod_get_csif_props(dev->kmod.dev)',
     'panvk_get_csif_props(dev)', 1),
    ('src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c',
     'panthor_kmod_get_csif_props(dev->kmod.dev)',
     'panvk_get_csif_props(dev)', 1),
    ('src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c',
     'panthor_kmod_get_csif_props(dev->kmod.dev)',
     'panvk_get_csif_props(dev)', 1),
    ('src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c',
     'panthor_kmod_get_flush_id(dev->kmod.dev)',
     'panvk_get_flush_id(dev)', 3),
]

# sanity: beta must show the helper at each site
for path, old, new, count in SUBS:
    bt = open(f'{B}/{path}', errors='replace').read()
    assert bt.count(new) >= 1, (path, new)
    mt = open(f'{M}/{path}', errors='replace').read()
    assert mt.count(old) >= count, (path, old, mt.count(old))

parts = []
for path, old, new, count in SUBS:
    src = open(f'{M}/{path}', errors='replace').read().splitlines(keepends=True)
    out = []
    done = 0
    for i, l in enumerate(src):
        if old in l and done < count:
            pre = l[:l.index(old)]
            # keep line structure, swap callee
            out.append(pre + new + l[l.index(old) + len(old):])
            done += 1
        else:
            out.append(l)
    assert done == count, (path, done, count)
    tmp = f'/tmp/009-{path.replace("/", "_")}'
    open(tmp, 'w').write(''.join(out))
    p = subprocess.run(['diff', '-U3', f'{M}/{path}', tmp],
                       capture_output=True, text=True)
    assert p.returncode == 1
    d = p.stdout.splitlines(keepends=True)
    d = [('--- a/' + path + '\n') if l.startswith('--- ')
         else (('+++ b/' + path + '\n') if l.startswith('+++ ') else l)
         for l in d]
    parts += d

header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH csf 009] panvk: route csif/flush-id sites via kbase helpers

Purpose: Call panvk_get_csif_props()/panvk_get_flush_id() (which select
 the kbase GLB interface / USER registers on /dev/mali* devices) at the
 remaining direct panthor_kmod_* call sites: cmd_draw, exception_handler
 (the vkCreateDevice crash: garbage cs_reg_count blew up the tiler-OOM
 handler builder), utrace, cmd_buffer, gpu_queue (4 sites). These hunks
 contain no 'kbase' string so the 006 extractor missed them. The utrace
 clone-allocator refactor drift is deliberately NOT taken (one line only).
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4 (call-site lines only).
Tested GPU: Poco X6 Pro / G615 (fixes vkCreateDevice crash).
Tested Kbase UAPI: CSF 1.21.
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: 006 (helpers defined in panvk_device.h).
Validation test: Gates D+E (vkCreateDevice + compute 10/10).

"""
open('patches/csf/009-csif-helper-callsites.patch', 'w').write(
    header + ''.join(parts))
print('wrote 009')
