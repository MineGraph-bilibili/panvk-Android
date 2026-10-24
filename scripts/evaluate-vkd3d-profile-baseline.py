#!/usr/bin/env python3
"""P19: re-evaluate vkd3d-proton 2.14.1 and 3.0.1. Does not flip features.

Keeps HARD_REQUIREMENTS, PROFILE_BASELINE, DEVICE_CREATE, and FEATURE_LEVEL
separate. Never merges them into one PASS.
"""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPS_PATH = ROOT / "validation/g615-v11-csf/consumer-capabilities.json"
PROFILES = {
    "2.14.1": ROOT / "validation/g615-v11-csf/profiles/vkd3d-2.14.1.json",
    "3.0.1": ROOT / "validation/g615-v11-csf/profiles/vkd3d-3.0.1.json",
}
JSON_OUT = ROOT / "validation/g615-v11-csf/p19-vkd3d-profile-baseline.json"
MD_OUT = ROOT / "validation/g615-v11-csf/P19-VKD3D-PROFILE-BASELINE.md"
TXT_OUT = ROOT / "validation/g615-v11-csf/p19-vkd3d-profile-baseline-2026-09-20.txt"
BASELINE_PROFILE = "VP_D3D12_FL_11_0_baseline"
FEATURE_LEVEL_PROFILES = (
    "VP_D3D12_FL_11_0_baseline",
    "VP_D3D12_FL_11_1_baseline",
    "VP_D3D12_FL_12_0_baseline",
    "VP_D3D12_FL_12_1_baseline",
    "VP_D3D12_FL_12_2_baseline",
)
UAB_LIMITS = (
    "maxDescriptorSetUpdateAfterBindStorageBuffers",
    "maxDescriptorSetUpdateAfterBindSampledImages",
    "maxDescriptorSetUpdateAfterBindStorageImages",
    "maxDescriptorSetUpdateAfterBindSamplers",
    "maxPerStageDescriptorUpdateAfterBindStorageBuffers",
    "maxPerStageDescriptorUpdateAfterBindSampledImages",
    "maxPerStageDescriptorUpdateAfterBindStorageImages",
    "maxPerStageDescriptorUpdateAfterBindSamplers",
)
INDEXING_EXCLUDED = {"descriptorBindingUniformBufferUpdateAfterBind"}
VKD3D_REPO = "https://github.com/HansKristian-Work/vkd3d-proton"
UAB_MINIMUM = 1000000

CLASSIFICATION = {
    "geometryShader": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; v11 XML has no GS stage (P12 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete Valhall v11 GS or PanVK poly/compute lowering, then CTS. Never expose early.",
    },
    "fillModeNonSolid": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no Valhall polygon-mode field (P11 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Post-vertex primitive expansion, then CTS. Never expose early.",
    },
    "shaderClipDistance": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no PanVK clip lowering (P10 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Primitive-stage clip, then pixel matrix and CTS. Never expose early.",
    },
    "shaderCullDistance": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no PanVK cull lowering (P10 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Primitive-stage cull, then pixel matrix and CTS. Never expose early.",
    },
    "textureCompressionBC": {
        "upstreamPanVK": "Hardware-dependent; G615 BC mask is 0. No complete PanVK emulation (P9 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "UNSUPPORTED_NATIVE",
        "action": "Keep false until transparent BC contract PASSes. Never advertise early.",
    },
    "multiViewport": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; v11 has no viewport array (P14 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Hardware proof of v11 ViewportIndex plus per-viewport transform/scissor/depth, then CTS. Never expose early.",
    },
    "transformFeedback": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; PanVK omits EXT_transform_feedback (P15 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete no-IDVS XFB variant, then CTS. Never expose early.",
    },
    "geometryStreams": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; XFB streams need GS (P15 BLOCKED_SAFE_FALSE, waits on P12)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete XFB plus geometryShader, then CTS. Never expose early.",
    },
    "VK_EXT_transform_feedback": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; PanVK get_device_extensions omits it (P15 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. Complete no-IDVS XFB variant, then CTS. Never expose early.",
    },
    "transformFeedbackQueries": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; XFB queries need EXT_transform_feedback (P15 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete XFB queries after transformFeedback, then CTS. Never expose early.",
    },
    "tessellationShader": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; v11 XML has no TCS/TES stage (P17 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete Valhall v11 TCS/TES or PanVK poly/compute lowering, then CTS. Never expose early.",
    },
    "pipelineStatisticsQuery": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; P20 not started",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Implement pipeline statistics with correct Vulkan counters, then CTS. Never expose early.",
    },
    "robustImageAccess2": {
        "upstreamPanVK": "Pinned and origin/main e1f3f372 still report false (P6 PARTIAL / UNSUPPORTED)",
        "implementationStatus": "UNSUPPORTED",
        "action": "Keep false. Complete robustImageAccess2 semantics, then CTS. Never expose early.",
    },
    "bufferImageGranularity": {
        "upstreamPanVK": "G615 reports 64. Profile lists 65536 as a numeric property. Smaller granularity is more capable.",
        "implementationStatus": "EVALUATOR_DIRECTION",
        "action": "No driver change. Existing evaluator uses >=; actual 64 already satisfies a 65536 max-granularity.",
    },
    "subgroupSupportedOperations[0]": {
        "upstreamPanVK": "G615 subgroupSupportedOperations=1791 includes VK_SUBGROUP_FEATURE_BASIC_BIT. Capture stores a bitmask, not a string array.",
        "implementationStatus": "EVALUATOR_ARRAY",
        "action": "No driver change. Evaluator compares flattened array string against integer bitmask.",
    },
    "VK_EXT_descriptor_buffer": {
        "upstreamPanVK": "Not advertised. README marks it highly recommended, not mandatory.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Do not make descriptor_buffer mandatory for the first compliance gate.",
    },
}


def load_evaluator():
    spec = importlib.util.spec_from_file_location(
        "evaluate_consumer_profile",
        ROOT / "scripts/evaluate-consumer-profile.py",
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def decode_hex_cstring(value):
    if not isinstance(value, str):
        return value
    try:
        raw = bytes.fromhex(value)
    except ValueError:
        return value
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def leaf(path):
    return path.rsplit(".", 1)[-1]


def classify(name, actual):
    known = CLASSIFICATION.get(name)
    if known:
        return dict(known)
    if actual is True:
        return {
            "upstreamPanVK": "Present in pinned G615 capture",
            "implementationStatus": "IMPLEMENTED",
            "action": "None",
        }
    if isinstance(actual, int) and actual > 0 and name not in CLASSIFICATION:
        return {
            "upstreamPanVK": "Present in pinned G615 capture",
            "implementationStatus": "IMPLEMENTED",
            "action": "None",
        }
    if actual is None:
        return {
            "upstreamPanVK": "UNKNOWN",
            "implementationStatus": "UNKNOWN",
            "action": "Extend capture before claiming support",
        }
    return {
        "upstreamPanVK": "Pinned runtime does not satisfy",
        "implementationStatus": "NOT_SATISFIED",
        "action": "Investigate in the owning feature phase; never expose early",
    }


def requirement_row(name, actual, required, source_version, source, status):
    row = {
        "requirement": name,
        "current": actual,
        "required": required,
        "status": status,
        "sourceVersion": source_version,
        "source": source,
    }
    row.update(classify(name, actual))
    if status == "PASS" and row["implementationStatus"] not in {
        "IMPLEMENTED",
        "EVALUATOR_DIRECTION",
        "EVALUATOR_ARRAY",
    }:
        row["implementationStatus"] = "IMPLEMENTED"
        row["upstreamPanVK"] = "Present in pinned G615 capture"
        row["action"] = "None"
    return row


def extension_present(caps, name):
    for item in caps.get("extensions", []):
        if item.get("name") == name:
            return item.get("specVersion", 0)
    return None


def api_tuple(caps):
    value = caps["device"]["apiVersion"]
    return ((value >> 22) & 0x7F, (value >> 12) & 0x3FF, value & 0xFFF)


def api_text(caps):
    major, minor, patch = api_tuple(caps)
    return f"{major}.{minor}.{patch}"


def uab_value(caps, name):
    props = caps.get("properties", {})
    v12 = caps.get("propertyStructures", {}).get("VkPhysicalDeviceVulkan12Properties", {})
    dip = caps.get("propertyStructures", {}).get(
        "VkPhysicalDeviceDescriptorIndexingProperties", {}
    )
    for source in (props, v12, dip):
        if name in source and source[name] is not None:
            return source[name]
    return None


def indexing_features(caps):
    return caps.get("featureStructures", {}).get(
        "VkPhysicalDeviceDescriptorIndexingFeatures", {}
    )


def feature_value(caps, name):
    features = caps.get("features", {})
    if name in features:
        return features[name]
    for struct in caps.get("featureStructures", {}).values():
        if name in struct:
            return struct[name]
    return None


def evaluate_hard(caps, version):
    source = (
        f"v{version} README.md hard requirements "
        "(Vulkan 1.3, descriptor indexing, 1_000_000 UpdateAfterBind except UniformBuffer, "
        "VkPhysicalDeviceDescriptorIndexingFeatures, samplerMirrorClampToEdge, "
        "shaderDrawParameters, VK_EXT_robustness2, VK_KHR_push_descriptor)"
    )
    rows = []
    api_ok = api_tuple(caps) >= (1, 3, 0)
    rows.append(
        requirement_row(
            "apiVersion>=1.3",
            api_text(caps),
            "1.3",
            version,
            source,
            "PASS" if api_ok else "FAIL",
        )
    )
    indexing_ext = extension_present(caps, "VK_EXT_descriptor_indexing")
    descriptor_indexing = feature_value(caps, "descriptorIndexing")
    rows.append(
        requirement_row(
            "descriptorIndexing",
            descriptor_indexing if descriptor_indexing is not None else indexing_ext,
            True,
            version,
            source,
            "PASS" if descriptor_indexing is True or indexing_ext is not None else "FAIL",
        )
    )
    idx = indexing_features(caps)
    missing = [
        name
        for name, value in idx.items()
        if name not in INDEXING_EXCLUDED and value is not True
    ]
    rows.append(
        requirement_row(
            "VkPhysicalDeviceDescriptorIndexingFeatures except UniformBuffer UAB",
            not missing,
            True,
            version,
            source,
            "PASS" if idx and not missing else "FAIL",
        )
    )
    for name in UAB_LIMITS:
        actual = uab_value(caps, name)
        status = "PASS" if actual is not None and actual >= UAB_MINIMUM else "FAIL"
        rows.append(requirement_row(name, actual, UAB_MINIMUM, version, source, status))
    for name, required in (
        ("samplerMirrorClampToEdge", True),
        ("shaderDrawParameters", True),
    ):
        actual = feature_value(caps, name)
        rows.append(
            requirement_row(
                name,
                actual,
                required,
                version,
                source,
                "PASS" if actual is True else "FAIL",
            )
        )
    for extension in ("VK_EXT_robustness2", "VK_KHR_push_descriptor"):
        actual = extension_present(caps, extension)
        rows.append(
            requirement_row(
                extension,
                actual,
                1,
                version,
                source,
                "PASS" if actual is not None else "FAIL",
            )
        )
    fails = [row for row in rows if row["status"] == "FAIL"]
    recommended = []
    for extension in (
        "VK_EXT_image_view_min_lod",
        "VK_EXT_mutable_descriptor_type",
        "VK_VALVE_mutable_descriptor_type",
        "VK_EXT_descriptor_buffer",
    ):
        actual = extension_present(caps, extension)
        recommended.append(
            requirement_row(
                extension,
                actual,
                1,
                version,
                f"v{version} README.md highly recommended, not mandatory",
                "PASS" if actual is not None else "FAIL",
            )
        )
    return {
        "status": "PASS" if not fails else "FAIL",
        "sourceKind": "readme-hard-requirements",
        "sourceFile": f"https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/v{version}/README.md",
        "source": source,
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "recommended": recommended,
        "note": (
            "README lists VK_EXT_robustness2 as an extension. "
            "robustImageAccess2 is a DEVICE_CREATE check in vkd3d_init_device_caps, not this HARD gate."
        ),
    }


def evaluate_device_create(caps, version):
    source = (
        f"v{version} libs/vkd3d/device.c vkd3d_init_device_caps "
        "(samplerMirrorClampToEdge, robustBufferAccess2, robustImageAccess2, "
        "nullDescriptor, shaderDrawParameters, VK_KHR_push_descriptor)"
    )
    checks = [
        ("apiVersion>=1.3", api_text(caps), "1.3", api_tuple(caps) >= (1, 3, 0)),
        (
            "samplerMirrorClampToEdge",
            feature_value(caps, "samplerMirrorClampToEdge"),
            True,
            feature_value(caps, "samplerMirrorClampToEdge") is True,
        ),
        (
            "shaderDrawParameters",
            feature_value(caps, "shaderDrawParameters"),
            True,
            feature_value(caps, "shaderDrawParameters") is True,
        ),
        (
            "VK_EXT_robustness2",
            extension_present(caps, "VK_EXT_robustness2"),
            1,
            extension_present(caps, "VK_EXT_robustness2") is not None,
        ),
        (
            "robustBufferAccess2",
            feature_value(caps, "robustBufferAccess2"),
            True,
            feature_value(caps, "robustBufferAccess2") is True,
        ),
        (
            "robustImageAccess2",
            feature_value(caps, "robustImageAccess2"),
            True,
            feature_value(caps, "robustImageAccess2") is True,
        ),
        (
            "nullDescriptor",
            feature_value(caps, "nullDescriptor"),
            True,
            feature_value(caps, "nullDescriptor") is True,
        ),
        (
            "VK_KHR_push_descriptor",
            extension_present(caps, "VK_KHR_push_descriptor"),
            1,
            extension_present(caps, "VK_KHR_push_descriptor") is not None,
        ),
    ]
    rows = [
        requirement_row(name, actual, required, version, source, "PASS" if ok else "FAIL")
        for name, actual, required, ok in checks
    ]
    fails = [row for row in rows if row["status"] == "FAIL"]
    smoke_reason = (
        f"Stock vkd3d-proton v{version} vkd3d_init_device_caps requires robustImageAccess2. "
        "G615 reports that bit false, so D3D12 device creation returns E_INVALIDARG. "
        "Stock vkd3d-proton is a Windows D3D12-on-Vulkan layer; this Android ICD is not a "
        "legal host for the tagged binaries. No stock vkd3d binary was launched."
    )
    return {
        "status": "PASS" if not fails else "FAIL",
        "sourceKind": "vkd3d-device-create",
        "sourceFile": f"https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/v{version}/libs/vkd3d/device.c",
        "source": source,
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": smoke_reason,
    }


def evaluate_profile(caps, evaluator, version, profile_name):
    imported = json.loads(PROFILES[version].read_text())
    result = evaluator.evaluate(caps, imported)
    provenance = imported["provenance"]
    profile = imported["document"]["profiles"][profile_name]
    evaluated = result["profiles"][profile_name]
    source = f"{provenance['tag']} VP_D3D12_VKD3D_PROTON_profile.json {profile_name}"
    rows = []
    for item in evaluated["pass"] + evaluated["fail"]:
        status = "PASS" if evaluator.item_satisfies(item) else "FAIL"
        rows.append(
            requirement_row(
                leaf(item["path"]),
                item["actual"],
                item["required"],
                version,
                f"{source} / {item['capability']} / {item['path']}",
                status,
            )
        )
    fails = [row for row in rows if row["status"] == "FAIL"]
    return {
        "label": profile.get("label", profile_name),
        "apiVersion": profile.get("api-version"),
        "capabilities": profile.get("capabilities"),
        "status": evaluated["status"],
        "passCount": len(evaluated["pass"]),
        "failCount": len(evaluated["fail"]),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "featureLevel": evaluated.get("featureLevel"),
        "source": source,
    }


def evaluate_version(caps, evaluator, version):
    imported = json.loads(PROFILES[version].read_text())
    provenance = imported["provenance"]
    hard = evaluate_hard(caps, version)
    device = evaluate_device_create(caps, version)
    evaluated_levels = {
        name: evaluate_profile(caps, evaluator, version, name)
        for name in FEATURE_LEVEL_PROFILES
    }
    baseline = evaluated_levels[BASELINE_PROFILE]
    feature_levels = {
        name: {
            "status": item["status"],
            "failCount": item["failCount"],
            "passCount": item["passCount"],
            "blockers": item["blockers"],
        }
        for name, item in evaluated_levels.items()
    }
    return {
        "vkd3dVersion": version,
        "tag": provenance["tag"],
        "repository": VKD3D_REPO,
        "sourceKind": "official-profile",
        "sourceFile": str(PROFILES[version].relative_to(ROOT)),
        "sourceProfile": BASELINE_PROFILE,
        "source": provenance["url"],
        "provenance": provenance,
        "HARD_REQUIREMENTS": hard,
        "PROFILE_BASELINE": baseline,
        "DEVICE_CREATE": device,
        "FEATURE_LEVEL": {
            "status": "NOT_AVAILABLE",
            "reason": (
                "D3D_FEATURE_LEVEL is not available because DEVICE_CREATE fails. "
                "Stock vkd3d never reaches CheckFeatureSupport. Machine-evaluated "
                "FL profiles remain FAIL and are recorded separately."
            ),
            "d3dFeatureLevel": "NOT_AVAILABLE",
            "profiles": feature_levels,
        },
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": device["smokeReason"],
    }


def driver_identity(caps):
    packed = caps["device"]["driverVersion"]
    driver = caps.get("propertyStructures", {}).get("VkPhysicalDeviceDriverProperties", {})
    return {
        "adapter": caps["device"]["name"],
        "vendorID": caps["device"]["vendorID"],
        "deviceID": caps["device"]["deviceID"],
        "deviceIDHex": f"0x{caps['device']['deviceID']:08x}",
        "driverVersionPacked": packed,
        "driverVersionHex": f"0x{packed:08x}",
        "driverVersionVk": {
            "major": (packed >> 22) & 0x7F,
            "minor": (packed >> 12) & 0x3FF,
            "patch": packed & 0xFFF,
        },
        "apiVersion": caps["device"]["apiVersion"],
        "apiVersionText": api_text(caps),
        "driverID": driver.get("driverID"),
        "driverName": decode_hex_cstring(driver.get("driverName")),
        "driverInfo": decode_hex_cstring(driver.get("driverInfo")),
    }


def md_table(rows, failures_only=False):
    selected = [row for row in rows if (row["status"] == "FAIL") or not failures_only]
    if not selected:
        return "None.\n"
    lines = [
        "| Requirement | Current | Required | Status | Source version | Source file/profile | Upstream PanVK | Implementation | Action |",
        "|---|---:|---:|---|---|---|---|---|---|",
    ]
    for row in selected:
        lines.append(
            "| `{requirement}` | `{current}` | `{required}` | {status} | `{sourceVersion}` | `{source}` | {upstreamPanVK} | {implementationStatus} | {action} |".format(
                **row
            )
        )
    return "\n".join(lines) + "\n"


def render_md(doc):
    driver = doc["driver"]
    lines = [
        "# P19 vkd3d-proton 2.14.1 / 3.0.1 Re-evaluation",
        "",
        "## Result",
        "",
        f"`{doc['result']}`",
        "",
        "Machine-evaluated HARD_REQUIREMENTS, PROFILE_BASELINE, DEVICE_CREATE, and FEATURE_LEVEL only.",
        "These four gates stay separate. No merged PASS. No PanVK patch. No feature-bit change. P20 was not started.",
        "",
        "```text",
        f"VKD3D_2_14_1_HARD={doc['versions']['2.14.1']['HARD_REQUIREMENTS']['status']}",
        f"VKD3D_2_14_1_BASELINE={doc['versions']['2.14.1']['PROFILE_BASELINE']['status']}",
        f"VKD3D_2_14_1_DEVICE={doc['versions']['2.14.1']['DEVICE_CREATE']['status']}",
        f"VKD3D_2_14_1_FEATURE_LEVEL={doc['versions']['2.14.1']['FEATURE_LEVEL']['d3dFeatureLevel']}",
        f"VKD3D_2_14_1_SMOKE={doc['versions']['2.14.1']['stockVkd3dSmoke']}",
        f"VKD3D_3_0_1_HARD={doc['versions']['3.0.1']['HARD_REQUIREMENTS']['status']}",
        f"VKD3D_3_0_1_BASELINE={doc['versions']['3.0.1']['PROFILE_BASELINE']['status']}",
        f"VKD3D_3_0_1_DEVICE={doc['versions']['3.0.1']['DEVICE_CREATE']['status']}",
        f"VKD3D_3_0_1_FEATURE_LEVEL={doc['versions']['3.0.1']['FEATURE_LEVEL']['d3dFeatureLevel']}",
        f"VKD3D_3_0_1_SMOKE={doc['versions']['3.0.1']['stockVkd3dSmoke']}",
        "```",
        "",
        "## Adapter",
        "",
        f"- Device: `{driver['adapter']}`",
        f"- Target: `{doc['capture']['target']}`",
        f"- VendorID: `{driver['vendorID']}`",
        f"- DeviceID: `{driver['deviceIDHex']}`",
        f"- Driver: `{driver['driverName']}` / `{driver['driverInfo']}`",
        f"- DriverID: `{driver['driverID']}` (`VK_DRIVER_ID_MESA_PANVK`)",
        f"- DriverVersion packed: `{driver['driverVersionPacked']}` (`{driver['driverVersionHex']}`)",
        f"- VK_VERSION decode: `{driver['driverVersionVk']['major']}.{driver['driverVersionVk']['minor']}.{driver['driverVersionVk']['patch']}` (packed git encoding; `driverInfo` is authoritative)",
        f"- Vulkan API: `{driver['apiVersionText']}`",
        f"- ICD SHA-256: `{doc['capture']['icdSha256']}`",
        f"- Capture: `{doc['capture']['command']}` at `{doc['capture']['timestamp']}`",
        "",
        "## Gate separation",
        "",
        "HARD_REQUIREMENTS comes from each tag's README. PROFILE_BASELINE comes from",
        f"`{BASELINE_PROFILE}` in `VP_D3D12_VKD3D_PROTON_profile.json`. DEVICE_CREATE comes from",
        "`vkd3d_init_device_caps` in `libs/vkd3d/device.c`. FEATURE_LEVEL is the D3D12",
        "feature level that stock vkd3d would report after a successful device create.",
        "",
        "README HARD can PASS while DEVICE_CREATE FAILs: README names `VK_EXT_robustness2`",
        "as an extension, but `vkd3d_init_device_caps` also requires `robustImageAccess2=true`.",
        "PROFILE_BASELINE is broader than either gate and still requires geometry, tessellation,",
        "XFB, BC, clip/cull, fillModeNonSolid, multiViewport, and pipelineStatisticsQuery.",
        "",
        "## Initialization / stock vkd3d smoke",
        "",
        "Stock vkd3d-proton tags `v2.14.1` and `v3.0.1` cannot legally create a D3D12 device",
        "on this ICD: `robustImageAccess2` remains `false`, so `vkd3d_init_device_caps`",
        "returns `E_INVALIDARG`. The tagged binaries are a Windows D3D12-on-Vulkan layer;",
        "this Android ICD is not a legal host. No game boot, no vkd3d init log, no reported",
        "D3D feature level. Smoke is `BLOCKED` for both versions.",
        "",
        "P6 left `robustImageAccess2` false. P9-P17 left `textureCompressionBC`,",
        "`shaderClipDistance`, `shaderCullDistance`, `fillModeNonSolid`, `geometryShader`,",
        "`multiViewport`, `transformFeedback`, `geometryStreams`, and `tessellationShader`",
        "false. P18 did not flip them. P19 did not flip them. `pipelineStatisticsQuery`",
        "remains false for P20.",
        "",
        "P21 D3D12 feature/profile analysis and P22 sparse/Kbase were not started.",
        "",
    ]
    for version in ("2.14.1", "3.0.1"):
        item = doc["versions"][version]
        hard = item["HARD_REQUIREMENTS"]
        baseline = item["PROFILE_BASELINE"]
        device = item["DEVICE_CREATE"]
        feature_level = item["FEATURE_LEVEL"]
        lines += [
            f"## vkd3d-proton {version}",
            "",
            f"HARD_REQUIREMENTS: **{hard['status']}**. PROFILE_BASELINE: **{baseline['status']}**. DEVICE_CREATE: **{device['status']}**. FEATURE_LEVEL: **{feature_level['d3dFeatureLevel']}**. Smoke: **{item['stockVkd3dSmoke']}**.",
            "",
            f"Official profile: `{item['source']}` (`{item['sourceFile']}`, `{item['provenance']['originalSha256']}`).",
            "",
            "### HARD_REQUIREMENTS",
            "",
            f"Source: `{hard['source']}`",
            "",
            hard["note"],
            "",
            f"Pass `{hard['passCount']}` / fail `{hard['failCount']}`.",
            "",
            md_table(hard["requirements"]),
            "Highly recommended, not mandatory:",
            "",
            md_table(hard["recommended"]),
            "### PROFILE_BASELINE",
            "",
            f"`{BASELINE_PROFILE}` (`{baseline['label']}`) is the official minimum D3D12 baseline. Higher FL / optimal / vendor-maximum profiles are recorded only under FEATURE_LEVEL evidence and are P21.",
            "",
            f"Pass `{baseline['passCount']}` / fail `{baseline['failCount']}`.",
            "",
            md_table(baseline["requirements"], failures_only=True),
            "### DEVICE_CREATE",
            "",
            f"Source: `{device['source']}`",
            "",
            device["smokeReason"],
            "",
            md_table(device["requirements"]),
            "### FEATURE_LEVEL",
            "",
            feature_level["reason"],
            "",
            "| Profile | Status | Pass | Fail |",
            "|---|---|---:|---:|",
        ]
        for name in FEATURE_LEVEL_PROFILES:
            fl = feature_level["profiles"][name]
            lines.append(
                f"| `{name}` | {fl['status']} | {fl['passCount']} | {fl['failCount']} |"
            )
        lines += ["", "Machine-evaluated FL profiles do not become a D3D feature level.", ""]
    lines += [
        "## Remaining blockers",
        "",
        "| Feature | Gate | Owner | Implementation |",
        "|---|---|---|---|",
        "| `robustImageAccess2` | DEVICE_CREATE | P6 | `UNSUPPORTED` |",
        "| `geometryShader` | PROFILE_BASELINE | P12 | `BLOCKED_SAFE_FALSE` |",
        "| `fillModeNonSolid` | PROFILE_BASELINE | P11 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderClipDistance` | PROFILE_BASELINE | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderCullDistance` | PROFILE_BASELINE | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `textureCompressionBC` | PROFILE_BASELINE | P9 | `BLOCKED_SAFE_FALSE` (`native=false`, `emulated=false`) |",
        "| `multiViewport` | PROFILE_BASELINE | P14 | `BLOCKED_SAFE_FALSE` |",
        "| `VK_EXT_transform_feedback` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `transformFeedback` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `geometryStreams` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` (also waits on P12) |",
        "| `transformFeedbackQueries` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `tessellationShader` | PROFILE_BASELINE | P17 | `BLOCKED_SAFE_FALSE` |",
        "| `pipelineStatisticsQuery` | PROFILE_BASELINE | P20 | `NOT_IMPLEMENTED` |",
        "| `bufferImageGranularity` | PROFILE_BASELINE evaluator | n/a | `EVALUATOR_DIRECTION` (actual 64 is finer than 65536) |",
        "| `subgroupSupportedOperations[0]` | PROFILE_BASELINE evaluator | n/a | `EVALUATOR_ARRAY` (bitmask 1791 includes BASIC) |",
        "",
        "## Checks",
        "",
        "- `scripts/evaluate-vulkan-profile.py` via `evaluate-consumer-profile.py` against `consumer-capabilities.json`.",
        "- Official `validation/g615-v11-csf/profiles/vkd3d-2.14.1.json` and `vkd3d-3.0.1.json` `VP_D3D12_FL_11_0_baseline`.",
        "- Tagged README hard-device requirements for `v2.14.1` and `v3.0.1`.",
        "- Tagged `libs/vkd3d/device.c` `vkd3d_init_device_caps` for DEVICE_CREATE.",
        "- `tests/dxvk-vkd3d/test_p19_vkd3d_profile_baseline.py`.",
        "",
        "## Exit",
        "",
        "P19 exits `FAIL`. HARD_REQUIREMENTS PASSes. PROFILE_BASELINE FAILs. DEVICE_CREATE FAILs on `robustImageAccess2`. FEATURE_LEVEL is `NOT_AVAILABLE`. Stock vkd3d smoke is `BLOCKED`.",
        "Next engineering step is P20 `pipelineStatisticsQuery` for remaining baseline bits, but DEVICE_CREATE stays blocked until `robustImageAccess2` is real. P21+ was not started.",
        "",
    ]
    return "\n".join(lines)


def render_txt(doc):
    lines = [
        "P19 vkd3d-proton 2.14.1 / 3.0.1 re-evaluation",
        f"result={doc['result']}",
        f"adapter={doc['driver']['adapter']}",
        f"driver={doc['driver']['driverName']} {doc['driver']['driverInfo']}",
        f"api={doc['driver']['apiVersionText']}",
        f"icd={doc['capture']['icdSha256']}",
    ]
    for version, item in doc["versions"].items():
        lines.append(
            f"vkd3d-{version} HARD={item['HARD_REQUIREMENTS']['status']} "
            f"BASELINE={item['PROFILE_BASELINE']['status']} "
            f"DEVICE={item['DEVICE_CREATE']['status']} "
            f"FEATURE_LEVEL={item['FEATURE_LEVEL']['d3dFeatureLevel']} "
            f"smoke={item['stockVkd3dSmoke']}"
        )
        lines.append(
            f"  HARD fail={item['HARD_REQUIREMENTS']['failCount']} blockers={','.join(item['HARD_REQUIREMENTS']['blockers']) or 'none'}"
        )
        lines.append(
            f"  BASELINE fail={item['PROFILE_BASELINE']['failCount']} blockers={','.join(item['PROFILE_BASELINE']['blockers'])}"
        )
        lines.append(
            f"  DEVICE fail={item['DEVICE_CREATE']['failCount']} blockers={','.join(item['DEVICE_CREATE']['blockers'])}"
        )
        lines.append(f"  {item['smokeReason']}")
    lines.append("P19_EXIT=FAIL")
    return "\n".join(lines) + "\n"


def main():
    evaluator = load_evaluator()
    caps = json.loads(CAPS_PATH.read_text())
    versions = {
        "2.14.1": evaluate_version(caps, evaluator, "2.14.1"),
        "3.0.1": evaluate_version(caps, evaluator, "3.0.1"),
    }
    doc = {
        "schemaVersion": 1,
        "phase": "P19",
        "date": "2026-09-20",
        "result": "FAIL",
        "smoke": "BLOCKED",
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "pinnedMesa": "5a07217f034b3e50d8c7c7794f97a2df1742613b",
        "auditedMesa": "e1f3f372c4a661cd0e71a0cdafc4d469d56ecf35",
        "capabilities": str(CAPS_PATH.relative_to(ROOT)),
        "evaluator": "scripts/evaluate-vulkan-profile.py",
        "capture": caps["capture"],
        "device": caps["device"],
        "driver": driver_identity(caps),
        "featureBitsUnchanged": {
            "geometryShader": caps["features"]["geometryShader"],
            "fillModeNonSolid": caps["features"]["fillModeNonSolid"],
            "shaderClipDistance": caps["features"]["shaderClipDistance"],
            "shaderCullDistance": caps["features"]["shaderCullDistance"],
            "textureCompressionBC": caps["features"]["textureCompressionBC"],
            "multiViewport": caps["features"]["multiViewport"],
            "transformFeedback": caps["features"]["transformFeedback"],
            "geometryStreams": caps["features"]["geometryStreams"],
            "tessellationShader": caps["features"]["tessellationShader"],
            "pipelineStatisticsQuery": caps["features"]["pipelineStatisticsQuery"],
            "robustImageAccess2": caps["features"]["robustImageAccess2"],
            "robustBufferAccess2": caps["features"]["robustBufferAccess2"],
            "nullDescriptor": caps["features"]["nullDescriptor"],
            "samplerMirrorClampToEdge": caps["features"]["samplerMirrorClampToEdge"],
            "shaderDrawParameters": caps["features"]["shaderDrawParameters"],
            "descriptorIndexing": caps["features"]["descriptorIndexing"],
        },
        "outOfScope": [
            "P20 pipelineStatisticsQuery implementation",
            "P21 D3D12 feature/profile analysis",
            "P22 sparse/Kbase",
            "P23 final matrix",
        ],
        "versions": versions,
        "gatesUnmerged": [
            "HARD_REQUIREMENTS",
            "PROFILE_BASELINE",
            "DEVICE_CREATE",
            "FEATURE_LEVEL",
        ],
    }
    JSON_OUT.write_text(json.dumps(doc, indent=2) + "\n")
    MD_OUT.write_text(render_md(doc))
    TXT_OUT.write_text(render_txt(doc))
    print(f"PASS output={JSON_OUT}")
    for version, item in versions.items():
        key = version.replace(".", "_")
        print(
            f"VKD3D_{key}_HARD={item['HARD_REQUIREMENTS']['status']} "
            f"BASELINE={item['PROFILE_BASELINE']['status']} "
            f"DEVICE={item['DEVICE_CREATE']['status']} "
            f"FEATURE_LEVEL={item['FEATURE_LEVEL']['d3dFeatureLevel']} "
            f"SMOKE={item['stockVkd3dSmoke']}"
        )
    print("P19_EXIT=FAIL")


if __name__ == "__main__":
    main()
