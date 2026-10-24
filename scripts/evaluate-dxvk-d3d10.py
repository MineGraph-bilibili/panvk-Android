#!/usr/bin/env python3
"""P16: re-evaluate DXVK D3D10 / D3D11 FL10.x against the G615 capture. Does not flip features."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPS_PATH = ROOT / "validation/g615-v11-csf/consumer-capabilities.json"
LEGACY_PATH = ROOT / "validation/requirements/dxvk-1.10.3.json"
PROFILES = {
    "2.7.1": ROOT / "validation/g615-v11-csf/profiles/dxvk-2.7.1.json",
    "3.1.1": ROOT / "validation/g615-v11-csf/profiles/dxvk-3.1.1.json",
}
JSON_OUT = ROOT / "validation/g615-v11-csf/p16-d3d10.json"
MD_OUT = ROOT / "validation/g615-v11-csf/P16-D3D10.md"
TXT_OUT = ROOT / "validation/g615-v11-csf/p16-d3d10-2026-09-20.txt"
OFFICIAL_D3D10_PROFILE = "VP_DXVK_d3d10_level_10_1_baseline"

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
    return row


def extension_present(caps, name):
    for item in caps.get("extensions", []):
        if item.get("name") == name:
            return item.get("specVersion", 0)
    return None


def evaluate_legacy(caps):
    legacy = json.loads(LEGACY_PATH.read_text())
    tier = legacy["tiers"]["D3D10_10_1"]
    source = f"{legacy['tag']} {tier['source']}"
    rows = []
    for feature in tier["requiredFeatures"]:
        actual = caps["features"].get(feature)
        status = "PASS" if actual is True else "FAIL"
        rows.append(requirement_row(feature, actual, True, legacy["version"], source, status))
    for extension in tier["requiredExtensions"]:
        actual = extension_present(caps, extension)
        status = "PASS" if actual is not None else "FAIL"
        rows.append(requirement_row(extension, actual, 1, legacy["version"], source, status))
    fails = [row for row in rows if row["status"] == "FAIL"]
    blockers = [row["requirement"] for row in fails]
    smoke_reason = (
        "Stock DXVK v1.10.3 enables required D3D10 / D3D11 FL10.x "
        "VkPhysicalDeviceFeatures and VK_EXT_transform_feedback at vkCreateDevice. "
        "G615 reports those features false and omits the extension, so device "
        "creation is illegal and init cannot complete. No stock DXVK binary was launched."
    )
    d3d10 = {
        "api": "D3D10",
        "sourceKind": "source-derived",
        "sourceFile": str(LEGACY_PATH.relative_to(ROOT)),
        "sourceProfile": "D3D10_10_1",
        "source": source,
        "status": "PASS" if not fails else "FAIL",
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "optionalFailCount": 0,
        "requirements": rows,
        "blockers": blockers,
    }
    d3d11 = dict(d3d10)
    d3d11["api"] = "D3D11_FL10_x"
    d3d11["sourceProfile"] = "D3D11_FL10_x"
    d3d11["source"] = (
        f"{legacy['tag']} {tier['source']} "
        "(D3D11 FL10.x uses the D3D10_10_1 enable path; FL11_0 extras start at d3d11_device.cpp:1994)"
    )
    return {
        "dxvkVersion": legacy["version"],
        "tag": legacy["tag"],
        "tagCommit": legacy["tagCommit"],
        "repository": legacy["repository"],
        "sourceKind": "source-derived",
        "sourceFile": str(LEGACY_PATH.relative_to(ROOT)),
        "status": "FAIL" if fails else "PASS",
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "stockDxvkSmoke": "BLOCKED",
        "passCount": d3d10["passCount"],
        "failCount": d3d10["failCount"],
        "optionalFailCount": 0,
        "d3d10": d3d10,
        "d3d11Fl10x": d3d11,
        "blockers": blockers,
        "smokeReason": smoke_reason,
    }


def evaluate_official(caps, evaluator, version):
    imported = json.loads(PROFILES[version].read_text())
    result = evaluator.evaluate(caps, imported)
    provenance = imported["provenance"]
    profile = imported["document"]["profiles"][OFFICIAL_D3D10_PROFILE]
    evaluated = result["profiles"][OFFICIAL_D3D10_PROFILE]
    source = f"{provenance['tag']} VP_DXVK_requirements.json {OFFICIAL_D3D10_PROFILE}"
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
    blockers = [row["requirement"] for row in fails]
    profile_block = {
        "label": profile.get("label", OFFICIAL_D3D10_PROFILE),
        "apiVersion": profile.get("api-version"),
        "capabilities": profile.get("capabilities"),
        "status": evaluated["status"],
        "passCount": len(evaluated["pass"]),
        "failCount": len(evaluated["fail"]),
        "optionalFailCount": 0,
        "requirements": rows,
        "blockers": blockers,
        "optionalFails": [],
    }
    smoke_reason = (
        f"Stock DXVK {provenance['tag']} D3D10 Level 10.1 / D3D11 FL10.x baseline requires "
        + ", ".join(blockers)
        + ". G615 reports those features false or the extension absent, so vkCreateDevice "
        "with the stock feature set is illegal. No stock DXVK binary was launched."
    )
    d3d10 = {
        "api": "D3D10",
        "sourceKind": "official-profile",
        "sourceFile": str(PROFILES[version].relative_to(ROOT)),
        "sourceProfile": OFFICIAL_D3D10_PROFILE,
        "source": provenance["url"],
        "status": evaluated["status"],
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "passCount": profile_block["passCount"],
        "failCount": profile_block["failCount"],
        "optionalFailCount": 0,
        "profiles": {OFFICIAL_D3D10_PROFILE: profile_block},
        "blockers": blockers,
    }
    d3d11 = dict(d3d10)
    d3d11["api"] = "D3D11_FL10_x"
    d3d11["source"] = (
        f"{provenance['url']} ({OFFICIAL_D3D10_PROFILE} is the FL10.x gate; "
        "D3D11 FL11_0 is P18 and was not evaluated)"
    )
    return {
        "dxvkVersion": version,
        "tag": provenance["tag"],
        "repository": "https://github.com/doitsujin/dxvk",
        "sourceKind": "official-profile",
        "sourceFile": str(PROFILES[version].relative_to(ROOT)),
        "sourceProfile": OFFICIAL_D3D10_PROFILE,
        "source": provenance["url"],
        "provenance": provenance,
        "status": evaluated["status"],
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "stockDxvkSmoke": "BLOCKED",
        "passCount": profile_block["passCount"],
        "failCount": profile_block["failCount"],
        "optionalFailCount": 0,
        "d3d10": d3d10,
        "d3d11Fl10x": d3d11,
        "blockers": blockers,
        "smokeReason": smoke_reason,
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
        "apiVersionText": "{}.{}.{}".format(
            (caps["device"]["apiVersion"] >> 22) & 0x7F,
            (caps["device"]["apiVersion"] >> 12) & 0x3FF,
            caps["device"]["apiVersion"] & 0xFFF,
        ),
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
            "| `{requirement}` | `{current}` | `{required}` | {status} | `{sourceVersion}` | `{source}` | {upstreamPanVK} | {implementationStatus} | {action} |".format(**row)
        )
    return "\n".join(lines) + "\n"


def render_md(doc):
    driver = doc["driver"]
    lines = [
        "# P16 DXVK D3D10 / D3D11 FL10.x Re-evaluation",
        "",
        "## Result",
        "",
        f"`{doc['result']}`",
        "",
        "Machine-evaluated D3D10 and D3D11 FL10.x only. No PanVK patch. No feature-bit change. P17 was not started.",
        "",
        "```text",
        f"DXVK_1_10_3_D3D10={doc['versions']['1.10.3']['d3d10']['status']}",
        f"DXVK_1_10_3_D3D11_FL10_1={doc['versions']['1.10.3']['d3d11Fl10x']['status']}",
        f"DXVK_1_10_3_SMOKE={doc['versions']['1.10.3']['stockDxvkSmoke']}",
        f"DXVK_2_7_1_D3D10={doc['versions']['2.7.1']['d3d10']['status']}",
        f"DXVK_2_7_1_D3D11_FL10_1={doc['versions']['2.7.1']['d3d11Fl10x']['status']}",
        f"DXVK_2_7_1_SMOKE={doc['versions']['2.7.1']['stockDxvkSmoke']}",
        f"DXVK_3_1_1_D3D10={doc['versions']['3.1.1']['d3d10']['status']}",
        f"DXVK_3_1_1_D3D11_FL10_1={doc['versions']['3.1.1']['d3d11Fl10x']['status']}",
        f"DXVK_3_1_1_SMOKE={doc['versions']['3.1.1']['stockDxvkSmoke']}",
        "D3D_FEATURE_LEVEL=NOT_AVAILABLE",
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
        "## Initialization / stock DXVK smoke",
        "",
        "Stock DXVK tags `v1.10.3`, `v2.7.1`, and `v3.1.1` cannot legally create a device on this ICD:",
        "required D3D10 / D3D11 FL10.x Vulkan features remain `false` and `VK_EXT_transform_feedback`",
        "is absent. `vkCreateDevice` with those bits is `VK_ERROR_FEATURE_NOT_PRESENT` or",
        "`VK_ERROR_EXTENSION_NOT_PRESENT`. No game boot, no DXVK init log, no reported D3D",
        "feature level. Smoke is `BLOCKED` for all three versions.",
        "",
        "P9-P15 left `textureCompressionBC`, `shaderClipDistance`, `shaderCullDistance`,",
        "`fillModeNonSolid`, `geometryShader`, `multiViewport`, `transformFeedback`, and",
        "`geometryStreams` false. P16 did not flip them.",
        "",
        "D3D11 FL11_0 (`tessellationShader` / `VP_DXVK_d3d11_level_11_0_baseline`) is P18 and was not evaluated.",
        "",
    ]
    v110 = doc["versions"]["1.10.3"]
    lines += [
        "## DXVK 1.10.3 D3D10 / D3D11 FL10.x",
        "",
        f"D3D10: **{v110['d3d10']['status']}**. D3D11 FL10.x: **{v110['d3d11Fl10x']['status']}**. Smoke: **{v110['stockDxvkSmoke']}**.",
        "",
        f"Source: `{v110['d3d10']['source']}` (`{v110['sourceFile']}`).",
        "",
        "D3D11 FL10.x uses the same D3D10_10_1 enable path. FL11_0 extras in `d3d11_device.cpp:1994-2004` are out of scope.",
        "",
        v110["smokeReason"],
        "",
        "### Required failures",
        "",
        md_table(v110["d3d10"]["requirements"], failures_only=True),
        "### All required D3D10 / FL10.x features",
        "",
        md_table(v110["d3d10"]["requirements"]),
    ]
    for version in ("2.7.1", "3.1.1"):
        item = doc["versions"][version]
        baseline = item["d3d10"]["profiles"][OFFICIAL_D3D10_PROFILE]
        lines += [
            f"## DXVK {version} D3D10 / D3D11 FL10.x",
            "",
            f"D3D10: **{item['d3d10']['status']}**. D3D11 FL10.x: **{item['d3d11Fl10x']['status']}**. Smoke: **{item['stockDxvkSmoke']}**.",
            "",
            f"Source: `{item['source']}` (`{item['sourceFile']}`, `{item['provenance']['originalSha256']}`).",
            "",
            f"`{OFFICIAL_D3D10_PROFILE}` is the official D3D10 Level 10.1 / D3D11 FL10.x gate. It uses `d3d11_baseline` and does not require tessellation.",
            "",
            item["smokeReason"],
            "",
            f"### {OFFICIAL_D3D10_PROFILE}: {baseline['status']}",
            "",
            f"Pass `{baseline['passCount']}` / fail `{baseline['failCount']}`. Optional does not apply.",
            "",
            md_table(baseline["requirements"], failures_only=True),
        ]
    lines += [
        "## Remaining blockers",
        "",
        "| Feature | Owner | Implementation |",
        "|---|---|---|",
        "| `geometryShader` | P12 | `BLOCKED_SAFE_FALSE` |",
        "| `fillModeNonSolid` | P11 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderClipDistance` | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderCullDistance` | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `textureCompressionBC` | P9 | `BLOCKED_SAFE_FALSE` (`native=false`, `emulated=false`) |",
        "| `multiViewport` | P14 | `BLOCKED_SAFE_FALSE` |",
        "| `VK_EXT_transform_feedback` | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `transformFeedback` | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `geometryStreams` | P15 | `BLOCKED_SAFE_FALSE` (also waits on P12) |",
        "",
        "## Checks",
        "",
        "- `scripts/evaluate-vulkan-profile.py` via `evaluate-consumer-profile.py` against `consumer-capabilities.json`.",
        "- Source-derived `validation/requirements/dxvk-1.10.3.json` D3D10_10_1 tier.",
        "- Official `validation/g615-v11-csf/profiles/dxvk-2.7.1.json` and `dxvk-3.1.1.json` `VP_DXVK_d3d10_level_10_1_baseline`.",
        "- `tests/dxvk-vkd3d/test_p16_d3d10_fl10_reevaluation.py`.",
        "",
        "## Exit",
        "",
        "P16 exits `FAIL`. All three D3D10 / D3D11 FL10.x evaluations fail. Stock DXVK smoke is `BLOCKED`.",
        "Next engineering step remains P9-P15 implementations. P17 `tessellationShader` was not started.",
        "",
    ]
    return "\n".join(lines)


def render_txt(doc):
    lines = [
        "P16 DXVK D3D10 / D3D11 FL10.x re-evaluation",
        f"result={doc['result']}",
        f"adapter={doc['driver']['adapter']}",
        f"driver={doc['driver']['driverName']} {doc['driver']['driverInfo']}",
        f"api={doc['driver']['apiVersionText']}",
        f"icd={doc['capture']['icdSha256']}",
    ]
    for version, item in doc["versions"].items():
        lines.append(
            f"dxvk-{version} D3D10={item['d3d10']['status']} D3D11_FL10_1={item['d3d11Fl10x']['status']} "
            f"smoke={item['stockDxvkSmoke']} fail={item['failCount']} blockers={','.join(item['blockers'])}"
        )
        lines.append(f"  feature_level={item['d3dFeatureLevel']}")
        lines.append(f"  {item['smokeReason']}")
    lines.append("P16_EXIT=FAIL")
    return "\n".join(lines) + "\n"


def main():
    evaluator = load_evaluator()
    caps = json.loads(CAPS_PATH.read_text())
    versions = {
        "1.10.3": evaluate_legacy(caps),
        "2.7.1": evaluate_official(caps, evaluator, "2.7.1"),
        "3.1.1": evaluate_official(caps, evaluator, "3.1.1"),
    }
    doc = {
        "schemaVersion": 1,
        "phase": "P16",
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
        },
        "outOfScope": ["P17 tessellationShader", "P18 D3D11 FL11_0"],
        "versions": versions,
    }
    JSON_OUT.write_text(json.dumps(doc, indent=2) + "\n")
    MD_OUT.write_text(render_md(doc))
    TXT_OUT.write_text(render_txt(doc))
    print(f"PASS output={JSON_OUT}")
    for version, item in versions.items():
        print(
            f"DXVK_{version.replace('.', '_')}_D3D10={item['d3d10']['status']} "
            f"D3D11_FL10_1={item['d3d11Fl10x']['status']} SMOKE={item['stockDxvkSmoke']}"
        )
    print("P16_EXIT=FAIL")


if __name__ == "__main__":
    main()
