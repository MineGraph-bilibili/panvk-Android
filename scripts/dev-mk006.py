#!/usr/bin/env python3
"""Assemble patch 006: device/queue/cmd kbase integration (filtered hunks)."""
import re, subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'

def udiff(a, b):
    p = subprocess.run(['diff', '-U3', a, b], capture_output=True, text=True)
    assert p.returncode in (0, 1), p.stderr
    return p.stdout

def kept_hunks_excl_sync_scope(diff_text):
    """kbase hunks, minus the create_cmdbuf sync_scope hunk: upstream pinned
    removed the cmdbuf->sync_scope member (CS scope is now per-op); the
    kbase SYSTEM-scope threading is deferred to queue-local logic in 007."""
    out = []
    for h in kept_hunks(diff_text):
        if 'sync_scope = MALI_CS_SYNC_SCOPE' in h:
            continue
        out.append(h)
    return out

def kept_hunks(diff_text, pattern='kbase'):
    lines = diff_text.splitlines(keepends=True)
    body = ''.join(l for l in lines if not l.startswith(('--- ', '+++ ')))
    hunks = [h for h in re.split(r'(?=@@ )', body) if h.strip()]
    return [h for h in hunks if re.search(pattern, h, re.I)]

def adapt_wsi_hunks(hunks):
    """Upstream pinned removed wsi_device_options.wait_present_before_queue
    and .x11_use_raw_fd_modifier (WSI refactor Sep 2026). Drop those two
    option assignments; keep sw_device gating, disable_unordered_submits
    and supports_modifiers. X11-on-kbase refinements deferred (Android
    surface is the P12 gate); kbase_dmabuf vars stay for modifier logic."""
    out = []
    for h in hunks:
        lines = []
        for l in h.splitlines(keepends=True):
            s = l.strip()
            if s.startswith(('+', ' ')) and (
                    '.wait_present_before_queue =' in s
                    or '.x11_use_raw_fd_modifier =' in s):
                if s.startswith(' '):
                    lines.append(l)  # keep context, must not happen here
                continue  # drop added option lines
            lines.append(l)
        out.append(''.join(lines))
    # recount @@ headers after line drops (counts are normative)
    fixed = []
    for h in out:
        ls = h.splitlines(keepends=True)
        old = sum(1 for l in ls[1:] if l.startswith((' ', '-')))
        new = sum(1 for l in ls[1:] if l.startswith((' ', '+')))
        m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@(.*)',
                     ls[0].rstrip('\n'))
        assert m, ls[0]
        ls[0] = f'@@ -{m.group(1)},{old} +{m.group(2)},{new} @@{m.group(3)}\n'
        fixed.append(''.join(ls))
    return fixed

def sectioned(path, hunks):
    out = [f'--- a/{path}\n', f'+++ b/{path}\n']
    out += hunks
    return out

FILES = [
    'src/panfrost/vulkan/panvk_device.h',
    'src/panfrost/vulkan/panvk_vX_device.c',
    'src/panfrost/vulkan/panvk_wsi.c',
    'src/panfrost/vulkan/csf/panvk_queue.h',
    'src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c',
    'src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c',
]
parts = []
for f in FILES:
    d = udiff(f'{M}/{f}', f'{B}/{f}')
    if f.endswith('panvk_vX_cmd_buffer.c'):
        k = kept_hunks_excl_sync_scope(d)
    elif f.endswith('panvk_wsi.c'):
        k = adapt_wsi_hunks(kept_hunks(d))
    else:
        k = kept_hunks(d)
    assert k, f
    parts += sectioned(f, k)

# cmd_buffer.h: progress enum + tiler estimate + decl, WITHOUT the CRC revert.
d = udiff(f'{M}/src/panfrost/vulkan/csf/panvk_cmd_buffer.h',
          f'{B}/src/panfrost/vulkan/csf/panvk_cmd_buffer.h')
k = [h for h in kept_hunks(d) if 'crc_header' not in h]
assert len(k) == 3, len(k)
parts += sectioned('src/panfrost/vulkan/csf/panvk_cmd_buffer.h', k)
parts += [
    '--- a/src/panfrost/vulkan/csf/panvk_cmd_buffer.h\n',
    '+++ b/src/panfrost/vulkan/csf/panvk_cmd_buffer.h\n',
    '@@ -195,6 +195,7 @@\n',
    '    } tiler_oom_ctx;\n',
    '    struct {\n',
    '       struct {\n',
    '          uint64_t cs;\n',
    '       } tracebuf;\n',
    '+   uint64_t kbase_progress_addr;\n',
    '    } debug;\n',
]

# vX_physical_device.c: ONLY the sparse-on-kbase gating (feature-truth:
# tessellation/fillMode/transform_feedback/outputLayer stay upstream until
# runtime-proven on the target GPU).
parts += [
    '--- a/src/panfrost/vulkan/panvk_vX_physical_device.c\n',
    '+++ b/src/panfrost/vulkan/panvk_vX_physical_device.c\n',
    '@@ -292,9 +292,12 @@\n',
    ' void\n',
    ' panvk_per_arch(get_physical_device_features)(\n',
    '    const struct panvk_instance *instance,\n',
    '    const struct panvk_physical_device *device, struct vk_features *features)\n',
    ' {\n',
    '-   bool has_sparse = PAN_ARCH >= 10;\n',
    '+   /* The kbase backend does not have a sparse bind queue implementation yet.\n',
    '+    * Do not advertise sparse support there, otherwise CTS will exercise sparse\n',
    '+    * binding paths that can only fail at submit time. */\n',
    '+   bool has_sparse = PAN_ARCH >= 10 && !device->kbase_node_path[0];\n',
    ' \n',
    '    *features = (struct vk_features){\n',
    '       /* Vulkan 1.0 */\n',
]

header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH kbase-common/csf 006] panvk: kbase queue/device/WSI integration

Purpose: Wire the kbase backend through device creation (fresh fd per
 logical device, AUTO_VA, sparse-bind stub queue), CSF queue/cmd-buffer
 state (userspace CS rings, seqno cells, progress markers), WSI
 (dmabuf-aware device options, modifier gating, ordered submits) and
 csif/flush-id helpers. Deliberately NOT included (kept upstream until
 runtime-proven on G615): tessellation/fillModeNonSolid/transform_feedback
 /shaderOutputLayer feature changes, cooperative_matrix/constant_data
 drift, CRC tracking removal, v11 arch-list drops.
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4, kbase-containing hunks only.
Tested GPU: none yet (compile-level).
Tested Kbase UAPI: n/a.
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: 003/004/005.
Sync-scope note: upstream pinned removed the cmdbuf->sync_scope member
 beta threads through draw/buffer sync sites; that hunk is dropped here
 and SYSTEM-scope selection for kbase moves to queue-local logic (007).
 Until then, intra-stream syncs use CSG scope.
Validation test: build with kbase; DRM path unchanged.

"""
open('patches/kbase-common/006-device-queue-wsi-kbase.patch', 'w').write(
    header + ''.join(parts))
print('wrote 006, sections:', len(parts))
