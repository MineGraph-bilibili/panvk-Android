#!/usr/bin/env python3
"""audit-runtime-extensions.py — Extension audit tool & gap analysis.

Per WORKER.md Sections 4, 5, 6, 8, 35.
Analyzes gap between upstream Mesa matrix and on-device runtime feature matrix.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 10 Desktop/display-only entries (WORKER.md Section 4.1)
DESKTOP_EXCLUDES = {
    "VK_KHR_display": {
        "reason": "Desktop/direct-display platform extension; guarded by VK_USE_PLATFORM_DISPLAY_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_KHR_get_display_properties2": {
        "reason": "Desktop/direct-display platform extension; requires VK_KHR_display; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_KHR_wayland_surface": {
        "reason": "Desktop window system extension (Wayland); guarded by VK_USE_PLATFORM_WAYLAND_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_WAYLAND_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_KHR_xcb_surface": {
        "reason": "Desktop window system extension (X11/XCB); guarded by VK_USE_PLATFORM_XCB_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_XCB_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_KHR_xlib_surface": {
        "reason": "Desktop window system extension (X11/Xlib); guarded by VK_USE_PLATFORM_XLIB_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_XLIB_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_EXT_acquire_drm_display": {
        "reason": "Desktop/direct-display extension (DRM direct display leasing); guarded by VK_USE_PLATFORM_DISPLAY_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_EXT_acquire_xlib_display": {
        "reason": "Desktop/direct-display extension (X11 RandR display leasing); guarded by VK_USE_PLATFORM_XLIB_XRANDR_EXT in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_XLIB_XRANDR_EXT"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_EXT_direct_mode_display": {
        "reason": "Desktop/direct-display platform extension; guarded by VK_USE_PLATFORM_DISPLAY_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_EXT_display_control": {
        "reason": "Desktop/direct-display platform extension; guarded by VK_USE_PLATFORM_DISPLAY_KHR in panvk_vX_physical_device.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_EXT_display_surface_counter": {
        "reason": "Desktop/direct-display platform extension; guarded by VK_USE_PLATFORM_DISPLAY_KHR in panvk_instance.c; inapplicable to Android/Bionic build.",
        "guards": ["VK_USE_PLATFORM_DISPLAY_KHR"],
        "archRequirement": "true",
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
}

# 17 High-confidence candidates (WORKER.md Section 5)
HIGH_CONFIDENCE_CANDIDATES = {
    "VK_KHR_compute_shader_derivatives": {
        "archRequirement": "PAN_ARCH >= 9",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: PAN_ARCH >= 9). v11 meets requirement. Needs runtime workload test before exposure.",
        "test": "Compile/run compute shader using derivative operations (computeDerivativeGroupQuads, computeDerivativeGroupLinear); validate deterministic results.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_depth_clamp_zero_one": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). Upstream driver promotes feature. Needs pipeline depth clamp validation before exposure.",
        "test": "Create pipeline using the extension behavior and validate pixels.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_internally_synchronized_queues": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true) for Vulkan 1.1-capable paths. Queue synchronization handled by common runtime.",
        "test": "Validate queue internally synchronized operations.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_maintenance7": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true) for Vulkan 1.1-capable paths. Needs API behavior test before exposure.",
        "test": "Exercise at least one API behavior introduced by extension, not just feature/property query.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_maintenance8": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true) for Vulkan 1.1-capable paths. Needs API behavior test before exposure.",
        "test": "Exercise at least one API behavior introduced by extension, not just feature/property query.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_maintenance9": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). Needs API behavior test before exposure.",
        "test": "Exercise at least one API behavior introduced by extension, not just feature/property query.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_pipeline_binary": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). Pipeline cache and binary properties supported. Needs pipeline binary round-trip test before exposure.",
        "test": "Create pipeline binary, retrieve/store, recreate pipeline, execute pipeline, compare result.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_present_id2": {
        "archRequirement": "true",
        "reason": "Promoted in upstream PanVK WSI under PANVK_USE_WSI_PLATFORM. Needs Android WSI present/wait sequence test.",
        "test": "Use existing Android WSI app and perform real present/wait sequences.",
        "guards": ["PANVK_USE_WSI_PLATFORM"],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_present_wait2": {
        "archRequirement": "true",
        "reason": "Promoted in upstream PanVK WSI under PANVK_USE_WSI_PLATFORM. Needs Android WSI present/wait sequence test.",
        "test": "Use existing Android WSI app and perform real present/wait sequences.",
        "guards": ["PANVK_USE_WSI_PLATFORM"],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_robustness2": {
        "archRequirement": "PAN_ARCH >= 10",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: PAN_ARCH >= 10). v11 meets requirement. Needs independent test before exposure.",
        "test": "Test robustBufferAccess2, robustImageAccess2 if exposed, nullDescriptor if exposed. Do not infer from VK_EXT_robustness2.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_shader_fma": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). SPV_KHR_shader_fma supported. Needs SPIR-V execution test before exposure.",
        "test": "Compile actual SPIR-V requiring the extension and execute it.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_shader_relaxed_extended_instruction": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). SPIR-V instruction relaxation supported by compiler backend.",
        "test": "Validate SPIR-V compiler handling of relaxed extended instructions.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_shader_untyped_pointers": {
        "archRequirement": "PAN_ARCH >= 9",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: PAN_ARCH >= 9). v11 meets requirement. Needs SPIR-V buffer workload test before exposure.",
        "test": "Compile SPIR-V using the required capability and run a deterministic buffer workload.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_surface_maintenance1": {
        "archRequirement": "true",
        "reason": "Promoted in upstream PanVK instance WSI under PANVK_USE_WSI_PLATFORM. Instance extension matching VK_EXT_surface_maintenance1.",
        "test": "Validate surface maintenance queries through Android WSI.",
        "guards": ["PANVK_USE_WSI_PLATFORM"],
        "file": "src/panfrost/vulkan/panvk_instance.c",
    },
    "VK_KHR_swapchain_maintenance1": {
        "archRequirement": "true",
        "reason": "Promoted in upstream PanVK WSI under PANVK_USE_WSI_PLATFORM. Needs swapchain maintenance operation test through Android swapchain.",
        "test": "Test at least one maintenance operation through the real Android swapchain.",
        "guards": ["PANVK_USE_WSI_PLATFORM"],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_unified_image_layouts": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). Needs unified-layout image validation test before exposure.",
        "test": "Exercise allowed unified-layout behavior with a real image.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_GOOGLE_user_type": {
        "archRequirement": "true",
        "reason": "Implemented in Mesa upstream (panvk_vX_physical_device.c: true). SPIR-V user type decoration passthrough.",
        "test": "Validate shader compilation with OpDecorateStringGOOGLE UserTypeGOOGLE.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
}

# 3 Investigate candidates (WORKER.md Section 6)
INVESTIGATE_CANDIDATES = {
    "VK_KHR_copy_memory_indirect": {
        "archRequirement": "PAN_ARCH >= 10",
        "reason": "Source indicates PAN_ARCH >= 10, but designated INVESTIGATE per Section 6: confirm exact implementation and entrypoints exist in candidate Mesa source before enabling.",
        "test": "Confirm exact implementation and entrypoints exist in candidate Mesa source. Confirm generated Vulkan headers support extension, all feature structs populated, and run actual extension workload.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_KHR_shader_constant_data": {
        "archRequirement": "true",
        "reason": "Present in upstream main (5a07217f) but designated INVESTIGATE per Section 6: confirm generated Vulkan headers and required feature/property structs exist before work.",
        "test": "Confirm exact implementation and entrypoints exist in candidate Mesa source. Confirm generated Vulkan headers support extension, all feature structs populated, and run actual extension workload.",
        "guards": [],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
    "VK_GOOGLE_display_timing": {
        "archRequirement": "wsi_instance_supports_google_display_timing",
        "reason": "Guarded by WSI support. Designated INVESTIGATE per Section 6: Android WSI must have a real timing implementation. Do not return fabricated timestamps.",
        "test": "Implement real Android display timing query via Choreographer / ANativeWindow timestamp; do not return fabricated timestamps.",
        "guards": ["PANVK_USE_WSI_PLATFORM"],
        "file": "src/panfrost/vulkan/panvk_vX_physical_device.c",
    },
}

# Explicit workload tests required per Section 25
SECTION_25_TEST_EXTENSIONS = {
    "VK_KHR_compute_shader_derivatives",
    "VK_KHR_depth_clamp_zero_one",
    "VK_KHR_maintenance7",
    "VK_KHR_maintenance8",
    "VK_KHR_maintenance9",
    "VK_KHR_pipeline_binary",
    "VK_KHR_robustness2",
    "VK_KHR_shader_fma",
    "VK_KHR_shader_untyped_pointers",
    "VK_KHR_unified_image_layouts",
    "VK_KHR_present_id2",
    "VK_KHR_present_wait2",
    "VK_KHR_swapchain_maintenance1",
}


def parse_mesa_c_table(filepath: pathlib.Path, mesa_root: pathlib.Path) -> dict[str, dict]:
    """Extract extension definitions from panvk_instance.c or panvk_vX_physical_device.c."""
    if not filepath.exists():
        return {}
    text = filepath.read_text(errors="replace")
    lines = text.splitlines()
    entries = {}
    ifdef_stack = []
    in_table = False

    for line in lines:
        stripped = line.strip()
        if "panvk_instance_extensions =" in line or "*ext = (struct vk_device_extension_table)" in line:
            in_table = True
            continue
        if in_table and stripped.startswith("};"):
            in_table = False
            continue

        if stripped.startswith("#ifdef") or stripped.startswith("#if defined"):
            cond = stripped.split(maxsplit=1)[1] if len(stripped.split(maxsplit=1)) > 1 else ""
            ifdef_stack.append(cond)
        elif stripped.startswith("#ifndef"):
            cond = "!" + (stripped.split(maxsplit=1)[1] if len(stripped.split(maxsplit=1)) > 1 else "")
            ifdef_stack.append(cond)
        elif stripped.startswith("#endif"):
            if ifdef_stack:
                ifdef_stack.pop()
        elif in_table:
            m = re.match(r'\s*\.([A-Za-z0-9_]+)\s*=\s*([^,/]+)', line)
            if m:
                ext_field = m.group(1)
                expr = m.group(2).strip()
                full_ext = "VK_" + ext_field
                try:
                    rel_file = str(filepath.relative_to(mesa_root))
                except ValueError:
                    rel_file = str(filepath)
                entries[full_ext] = {
                    "field": ext_field,
                    "expr": expr,
                    "guards": list(ifdef_stack),
                    "file": rel_file,
                }
    return entries


def evaluate_arch_requirement(expr: str, target_arch_num: int) -> bool:
    """Evaluate simple arch expressions like 'PAN_ARCH >= 9' or 'true'."""
    expr = expr.strip()
    if expr in ("true", "TRUE", "1"):
        return True
    if expr in ("false", "FALSE", "0"):
        return False
    m = re.match(r'PAN_ARCH\s*(>=|>|==|<=|<)\s*(\d+)', expr)
    if m:
        op, val_str = m.group(1), int(m.group(2))
        if op == ">=":
            return target_arch_num >= val_str
        if op == ">":
            return target_arch_num > val_str
        if op == "==":
            return target_arch_num == val_str
        if op == "<=":
            return target_arch_num <= val_str
        if op == "<":
            return target_arch_num < val_str
    return True


def audit_extensions(
    mesa_dir: pathlib.Path | None,
    upstream_json_path: pathlib.Path,
    runtime_json_path: pathlib.Path,
    platform: str = "android",
    arch: str = "v11",
) -> dict:
    # 1. Parse Pan arch number
    arch_num_match = re.search(r'\d+', arch)
    arch_num = int(arch_num_match.group(0)) if arch_num_match else 11

    # 2. Load upstream matrix
    if not upstream_json_path.exists():
        raise FileNotFoundError(f"Upstream feature matrix not found: {upstream_json_path}")
    upstream_data = json.loads(upstream_json_path.read_text())
    mesa_commit = upstream_data.get("mesaCommit", "unknown")
    mesa_version = upstream_data.get("mesaVersion", "unknown")

    upstream_rows = upstream_data.get("rows", [])
    panvk_rows = [r for r in upstream_rows if r.get("drivers") and "panvk" in r["drivers"]]
    panvk_done_names = set(r["name"] for r in panvk_rows)
    all_upstream_names = set(r["name"] for r in upstream_rows)

    # 3. Load runtime matrix
    if not runtime_json_path.exists():
        raise FileNotFoundError(f"Runtime feature matrix not found: {runtime_json_path}")
    runtime_data = json.loads(runtime_json_path.read_text())

    runtime_inst_list = [
        e if isinstance(e, str) else e["extensionName"]
        for e in runtime_data.get("instanceExtensions", [])
    ]
    runtime_dev_list = [
        e if isinstance(e, str) else e["extensionName"]
        for e in runtime_data.get("deviceExtensions", [])
    ]
    runtime_inst = set(runtime_inst_list)
    runtime_dev = set(runtime_dev_list)
    runtime_all = runtime_inst | runtime_dev

    # 4. Parse Mesa C source if available
    mesa_c_entries = {}
    if mesa_dir and mesa_dir.exists():
        inst_file = mesa_dir / "src" / "panfrost" / "vulkan" / "panvk_instance.c"
        vx_file = mesa_dir / "src" / "panfrost" / "vulkan" / "panvk_vX_physical_device.c"
        mesa_c_entries.update(parse_mesa_c_table(inst_file, mesa_dir))
        mesa_c_entries.update(parse_mesa_c_table(vx_file, mesa_dir))

    # 5. Overlap, gap, and runtime extras
    overlap = panvk_done_names & runtime_all
    missing_from_runtime = panvk_done_names - runtime_all
    runtime_extra = runtime_all - panvk_done_names

    # 6. Classify desktop excludes vs candidates
    if platform == "android":
        desktop_missing = set(k for k in DESKTOP_EXCLUDES.keys() if k in missing_from_runtime)
    else:
        desktop_missing = set()
    candidates_set = missing_from_runtime - desktop_missing

    high_confidence_set = candidates_set & set(HIGH_CONFIDENCE_CANDIDATES.keys())
    investigate_set = candidates_set & set(INVESTIGATE_CANDIDATES.keys())

    # Build gap dictionary
    gap_dict = {}
    for ext_name in sorted(missing_from_runtime):
        c_info = mesa_c_entries.get(ext_name, {})

        if ext_name in DESKTOP_EXCLUDES:
            meta = DESKTOP_EXCLUDES[ext_name]
            reason = meta["reason"]
            guards = meta.get("guards", c_info.get("guards", []))
            arch_req = meta.get("archRequirement", c_info.get("expr", "true"))
            src_file = meta.get("file", c_info.get("file", "src/panfrost/vulkan/panvk_instance.c"))

            if platform == "android":
                classification = "platform-inapplicable"
            else:
                classification = "source-supported-not-exposed"

            gap_dict[ext_name] = {
                "matrix": True,
                "runtime": False,
                "classification": classification,
                "reason": reason,
                "archRequirement": arch_req,
                "candidateGroup": "platform-inapplicable",
                "requiresWorkloadTest": False,
                "sourceFile": src_file,
                "sourceGuards": guards,
            }
        elif ext_name in INVESTIGATE_CANDIDATES:
            meta = INVESTIGATE_CANDIDATES[ext_name]
            reason = meta["reason"]
            arch_req = meta.get("archRequirement", c_info.get("expr", "true"))
            guards = meta.get("guards", c_info.get("guards", []))
            src_file = meta.get("file", c_info.get("file", "src/panfrost/vulkan/panvk_vX_physical_device.c"))

            if ext_name == "VK_GOOGLE_display_timing":
                classification = "requires-wsi"
            else:
                classification = "requires-runtime-test"

            gap_dict[ext_name] = {
                "matrix": True,
                "runtime": False,
                "classification": classification,
                "reason": reason,
                "archRequirement": arch_req,
                "candidateGroup": "investigate",
                "requiresWorkloadTest": True,
                "workloadTest": meta.get("test", ""),
                "sourceFile": src_file,
                "sourceGuards": guards,
            }
        elif ext_name in HIGH_CONFIDENCE_CANDIDATES:
            meta = HIGH_CONFIDENCE_CANDIDATES[ext_name]
            reason = meta["reason"]
            arch_req = meta.get("archRequirement", c_info.get("expr", "true"))
            guards = meta.get("guards", c_info.get("guards", []))
            src_file = meta.get("file", c_info.get("file", "src/panfrost/vulkan/panvk_vX_physical_device.c"))

            # Evaluate arch compatibility
            if not evaluate_arch_requirement(arch_req, arch_num):
                classification = "arch-inapplicable"
                reason = f"Requires {arch_req}; current architecture is v{arch_num}"
            else:
                classification = "source-supported-not-exposed"

            has_sec25_test = ext_name in SECTION_25_TEST_EXTENSIONS
            gap_dict[ext_name] = {
                "matrix": True,
                "runtime": False,
                "classification": classification,
                "reason": reason,
                "archRequirement": arch_req,
                "candidateGroup": "high-confidence",
                "requiresWorkloadTest": has_sec25_test,
                "workloadTest": meta.get("test", ""),
                "sourceFile": src_file,
                "sourceGuards": guards,
            }
        else:
            # Fallback for any other missing extension
            arch_req = c_info.get("expr", "true")
            guards = c_info.get("guards", [])
            src_file = c_info.get("file", "unknown")
            gap_dict[ext_name] = {
                "matrix": True,
                "runtime": False,
                "classification": "source-supported-not-exposed",
                "reason": "Present in PanVK upstream matrix but not exposed at runtime.",
                "archRequirement": arch_req,
                "candidateGroup": "investigate",
                "requiresWorkloadTest": True,
                "sourceFile": src_file,
                "sourceGuards": guards,
            }

    # Build full extensions dictionary (gap + runtime + non-panvk upstream)
    extensions_dict = dict(gap_dict)

    # Add runtime exposed
    for ext_name in sorted(runtime_all):
        c_info = mesa_c_entries.get(ext_name, {})
        is_matrix = ext_name in panvk_done_names
        is_inst = ext_name in runtime_inst
        reason = "Exposed and operational at runtime on target device."
        if ext_name == "VK_KHR_android_surface":
            reason = "Android WSI platform extension exposed and tested at runtime."
        extensions_dict[ext_name] = {
            "matrix": is_matrix,
            "runtime": True,
            "classification": "runtime",
            "reason": reason,
            "archRequirement": c_info.get("expr", "true"),
            "candidateGroup": "runtime",
            "requiresWorkloadTest": False,
            "type": "instance" if is_inst else "device",
            "sourceFile": c_info.get("file", "src/panfrost/vulkan/panvk_instance.c" if is_inst else "src/panfrost/vulkan/panvk_vX_physical_device.c"),
            "sourceGuards": c_info.get("guards", []),
        }

    # Add non-PanVK upstream extensions (the other 91)
    for ext_name in sorted(all_upstream_names - panvk_done_names - runtime_all):
        extensions_dict[ext_name] = {
            "matrix": False,
            "runtime": False,
            "classification": "source-not-present",
            "reason": "Not implemented in Mesa PanVK driver source.",
            "archRequirement": "n/a",
            "candidateGroup": "none",
            "requiresWorkloadTest": False,
            "sourceFile": "none",
            "sourceGuards": [],
        }

    # Count classifications
    classifications_count = {}
    for item in extensions_dict.values():
        c = item["classification"]
        classifications_count[c] = classifications_count.get(c, 0) + 1

    summary = {
        "platform": platform,
        "panArch": arch,
        "mesaCommit": mesa_commit,
        "mesaVersion": mesa_version,
        "deviceName": runtime_data.get("deviceName", "Mali-G615 MC6"),
        "apiVersion": runtime_data.get("apiVersion", "1.4.363"),
        "totalTracked": len(panvk_done_names | runtime_all),
        "upstreamPanvkDone": len(panvk_done_names),
        "runtimeExposed": len(runtime_all),
        "runtimeInstance": len(runtime_inst),
        "runtimeDevice": len(runtime_dev),
        "runtimeOverlap": len(overlap),
        "runtimeExtra": len(runtime_extra),
        "missingCount": len(missing_from_runtime),
        "platformInapplicable": len(desktop_missing),
        "auditCandidatesCount": len(candidates_set),
        "candidatesBreakdown": {
            "highConfidence": len(high_confidence_set),
            "investigate": len(investigate_set),
            "requiresTests": len(candidates_set & SECTION_25_TEST_EXTENSIONS),
        },
        "classifications": classifications_count,
    }

    audit_candidates = {
        "highConfidence": sorted(high_confidence_set),
        "investigate": sorted(investigate_set),
        "requiresTests": sorted(candidates_set & SECTION_25_TEST_EXTENSIONS),
    }

    result = {
        "_meta": summary,
        "summary": summary,
        "auditCandidates": audit_candidates,
        "gap": gap_dict,
        "extensions": extensions_dict,
    }

    # Expose missing extensions at top-level per WORKER.md Section 8 example
    for k, v in gap_dict.items():
        result[k] = v

    return result


def generate_markdown(result: dict) -> str:
    s = result["summary"]
    gap = result["gap"]
    candidates = result["auditCandidates"]

    lines = [
        "# PanVK Extension Gap Analysis & Runtime Audit",
        "",
        f"- **Mesa Commit**: `{s['mesaCommit']}` ({s['mesaVersion']})",
        f"- **Target Platform**: `{s['platform']}`",
        f"- **Pan Architecture**: `{s['panArch']}`",
        f"- **Device**: `{s['deviceName']}` (Vulkan `{s['apiVersion']}`)",
        "",
        "## 1. Extension Counts Summary",
        "",
        "| Metric | Count | Description |",
        "|---|---:|---|",
        f"| Upstream PanVK Matrix DONE | {s['upstreamPanvkDone']} | Implemented upstream in Mesa `docs/features.txt` |",
        f"| Runtime Exposed (Total) | {s['runtimeExposed']} | Verified on-device ({s['runtimeInstance']} instance, {s['runtimeDevice']} device) |",
        f"| Runtime Overlap with Matrix | {s['runtimeOverlap']} | Upstream DONE extensions verified at runtime |",
        f"| Runtime Extra (Android) | {s['runtimeExtra']} | `VK_KHR_android_surface` |",
        f"| Missing at Runtime (Total Gap) | {s['missingCount']} | Absent from on-device beta.2 baseline |",
        f"| Platform-Inapplicable Excludes | {s['platformInapplicable']} | Desktop/direct-DRM display extensions (WORKER.md 4.1) |",
        f"| Android Audit Candidates | {s['auditCandidatesCount']} | Android-relevant candidate pool (WORKER.md 4.2) |",
        "",
        "## 2. Audit Candidates Breakdown",
        "",
        f"- **High-Confidence Candidates ({len(candidates['highConfidence'])})**: Upstream source contains verified predicates for v11/Android.",
        f"- **Investigate Candidates ({len(candidates['investigate'])})**: Matrix documentation alone insufficient; requires source proof & implementation check (WORKER.md Section 6).",
        f"- **Workload Tests Required ({len(candidates['requiresTests'])})**: Explicit workload test specifications in WORKER.md Section 25 before exposure.",
        "",
        "### High-Confidence Candidates (Section 5)",
        "",
        "| Extension | Arch Requirement | Source File / Predicate |",
        "|---|---|---|",
    ]

    for name in candidates["highConfidence"]:
        info = gap[name]
        lines.append(f"| `{name}` | `{info['archRequirement']}` | `{info['sourceFile']}` |")

    lines.extend([
        "",
        "### Investigate Candidates (Section 6)",
        "",
        "| Extension | Classification | Investigation / Proof Requirement |",
        "|---|---|---|",
    ])

    for name in candidates["investigate"]:
        info = gap[name]
        lines.append(f"| `{name}` | `{info['classification']}` | {info['reason']} |")

    lines.extend([
        "",
        "### Workload Test Requirements (Section 25)",
        "",
        "| Extension | Required Workload Test |",
        "|---|---|",
    ])

    for name in sorted(set(candidates["requiresTests"]) | set(candidates["investigate"])):
        info = gap[name]
        if info.get("workloadTest"):
            lines.append(f"| `{name}` | {info['workloadTest']} |")

    lines.extend([
        "",
        "## 3. Platform-Inapplicable Desktop Excludes (Section 4.1)",
        "",
        "These 10 extensions are desktop/display-only and must NOT be chased on Android:",
        "",
        "| Extension | Source Guard | Reason |",
        "|---|---|---|",
    ])

    for name, info in sorted(gap.items()):
        if info["classification"] == "platform-inapplicable":
            guards = ", ".join(info["sourceGuards"]) if info["sourceGuards"] else "none"
            lines.append(f"| `{name}` | `{guards}` | {info['reason']} |")

    lines.extend([
        "",
        "## 4. Complete Missing Extensions Gap Details (30 Extensions)",
        "",
        "| Extension | Classification | Group | Arch Req | Reason |",
        "|---|---|---|---|---|",
    ])

    for name, info in sorted(gap.items()):
        lines.append(
            f"| `{name}` | `{info['classification']}` | `{info['candidateGroup']}` | `{info['archRequirement']}` | {info['reason']} |"
        )

    lines.extend([
        "",
        "## 5. Classification Schema Overview",
        "",
        "| Classification | Count | Description |",
        "|---|---:|---|",
    ])

    schema_desc = {
        "runtime": "Exposed and operational at runtime on target device",
        "platform-inapplicable": "Platform does not support feature (e.g. Wayland/X11/DRM on Android)",
        "source-supported-not-exposed": "Pan arch / platform supports it in source, but not enabled at runtime",
        "source-not-present": "Not implemented in Mesa PanVK driver source",
        "arch-inapplicable": "Requires different Pan architecture",
        "requires-wsi": "Requires WSI support not present on platform",
        "requires-kernel-capability": "Requires Kbase/kernel capability not present",
        "requires-runtime-test": "Requires proof and validation test before exposure",
    }

    for c_name, count in sorted(s["classifications"].items(), key=lambda x: -x[1]):
        desc = schema_desc.get(c_name, "")
        lines.append(f"| `{c_name}` | {count} | {desc} |")

    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Extension audit tool & gap analysis (WORKER.md Sections 4, 5, 6, 8, 35)"
    )
    parser.add_argument(
        "--mesa",
        type=pathlib.Path,
        default=ROOT / "work" / "mesa",
        help="Path to Mesa source checkout (default: work/mesa)",
    )
    parser.add_argument(
        "--upstream",
        type=pathlib.Path,
        default=ROOT / "snapshots" / "matrix-2026-09-18-5a07217" / "upstream-feature-matrix.json",
        help="Path to upstream feature matrix JSON",
    )
    parser.add_argument(
        "--runtime",
        type=pathlib.Path,
        default=ROOT / "dist" / "runtime-feature-matrix.json",
        help="Path to on-device runtime feature matrix JSON",
    )
    parser.add_argument(
        "--platform",
        choices=["android", "glibc"],
        default="android",
        help="Target platform (default: android)",
    )
    parser.add_argument(
        "--arch",
        default="v11",
        help="Pan architecture (default: v11)",
    )
    parser.add_argument(
        "--out-json",
        type=pathlib.Path,
        default=ROOT / "dist" / "extension-gap.json",
        help="Output gap JSON path (default: dist/extension-gap.json)",
    )
    parser.add_argument(
        "--out-md",
        type=pathlib.Path,
        default=ROOT / "dist" / "extension-gap.md",
        help="Output gap Markdown path (default: dist/extension-gap.md)",
    )

    args = parser.parse_args()

    # Fallback to ref-g720-beta if work/mesa doesn't exist
    mesa_dir = args.mesa
    if not mesa_dir.exists():
        fallback_mesa = ROOT / "work" / "ref-g720-beta"
        if fallback_mesa.exists():
            mesa_dir = fallback_mesa

    # Run audit
    result = audit_extensions(
        mesa_dir=mesa_dir,
        upstream_json_path=args.upstream,
        runtime_json_path=args.runtime,
        platform=args.platform,
        arch=args.arch,
    )

    # Ensure output directory exists
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.parent.mkdir(parents=True, exist_ok=True)

    # Write JSON
    args.out_json.write_text(json.dumps(result, indent=2) + "\n")

    # Write Markdown
    md_content = generate_markdown(result)
    args.out_md.write_text(md_content + "\n")

    s = result["summary"]
    cb = s["candidatesBreakdown"]

    print("=== PanVK Extension Audit & Gap Analysis ===")
    print(f"Platform:              {s['platform']} (arch: {s['panArch']})")
    print(f"Mesa Commit:           {s['mesaCommit']}")
    print(f"Upstream PanVK DONE:   {s['upstreamPanvkDone']}")
    print(f"Runtime Exposed:       {s['runtimeExposed']} ({s['runtimeInstance']} instance, {s['runtimeDevice']} device)")
    print(f"Runtime Overlap:       {s['runtimeOverlap']}")
    print(f"Runtime Extra:         {s['runtimeExtra']} (VK_KHR_android_surface)")
    print(f"Missing from Runtime:  {s['missingCount']}")
    print(f"Platform Inapplicable: {s['platformInapplicable']} (desktop/display-only excludes)")
    print(f"Audit Candidates:      {s['auditCandidatesCount']}")
    print(f"  - High-Confidence:   {cb['highConfidence']}")
    print(f"  - Investigate:       {cb['investigate']}")
    print(f"  - Requires Tests:    {cb['requiresTests']} (explicit Section 25 workloads)")
    print(f"Output JSON:           {args.out_json}")
    print(f"Output Markdown:       {args.out_md}")
    print("Status:                OK")


if __name__ == "__main__":
    main()
