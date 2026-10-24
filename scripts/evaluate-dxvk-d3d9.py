#!/usr/bin/env python3
"""P13: re-evaluate DXVK D3D9 against the G615 capture. Does not flip features."""
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
JSON_OUT = ROOT / "validation/g615-v11-csf/p13-d3d9.json"
MD_OUT = ROOT / "validation/g615-v11-csf/P13-D3D9.md"
TXT_OUT = ROOT / "validation/g615-v11-csf/p13-d3d9-2026-09-20.txt"

CLASSIFICATION = {
    "geometryShader": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; v11 XML has no GS stage (P12)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete Valhall v11 GS or PanVK poly/compute lowering, then CTS. Never expose early.",
    },
    "fillModeNonSolid": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no Valhall polygon-mode field (P11)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Post-vertex primitive expansion, then CTS. Never expose early.",
    },
    "shaderClipDistance": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no PanVK clip lowering (P10)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Primitive-stage clip, then pixel matrix and CTS. Never expose early.",
    },
    "shaderCullDistance": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; no PanVK cull lowering (P10)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Primitive-stage cull, then pixel matrix and CTS. Never expose early.",
    },
    "textureCompressionBC": {
        "upstreamPanVK": "Hardware-dependent; G615 BC mask is 0. No complete PanVK emulation (P9)",
        "implementationStatus": "UNSUPPORTED_NATIVE",
        "action": "Keep false until transparent BC contract PASSes. Never advertise early.",
    },
    "multiViewport": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; P14 not started",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P14. Never expose early.",
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


def evaluate_legacy(caps, evaluator):
    legacy = json.loads(LEGACY_PATH.read_text())
    tier = legacy["tiers"]["D3D9"]
    source = f"{legacy['tag']} {tier['source']}"
    rows = []
    for feature in tier["requiredFeatures"]:
        actual = caps["features"].get(feature)
        status = "PASS" if actual is True else "FAIL"
        rows.append(requirement_row(feature, actual, True, legacy["version"], source, status))
    fails = [row for row in rows if row["status"] == "FAIL"]
    return {
        "dxvkVersion": legacy["version"],
        "tag": legacy["tag"],
        "tagCommit": legacy["tagCommit"],
        "repository": legacy["repository"],
        "sourceKind": "source-derived",
        "sourceFile": str(LEGACY_PATH.relative_to(ROOT)),
        "sourceProfile": "D3D9",
        "source": source,
        "status": "PASS" if not fails else "FAIL",
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "stockDxvkSmoke": "BLOCKED",
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "optionalFailCount": 0,
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "smokeReason": (
            "Stock DXVK v1.10.3 enables required D3D9 VkPhysicalDeviceFeatures at "
            "vkCreateDevice. G615 reports them false, so device creation is illegal "
            "and init cannot complete. No stock DXVK binary was launched."
        ),
    }


def evaluate_official(caps, evaluator, version):
    imported = json.loads(PROFILES[version].read_text())
    result = evaluator.evaluate(caps, imported)
    provenance = imported["provenance"]
    profiles = {}
    for name, profile in imported["document"]["profiles"].items():
        if "d3d9" not in name.lower():
            continue
        evaluated = result["profiles"][name]
        source = f"{provenance['tag']} VP_DXVK_requirements.json {name}"
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
        optional_fail = [item for item in evaluated["optional"] if item.get("status") == "FAIL"]
        profiles[name] = {
            "label": profile.get("label", name),
            "apiVersion": profile.get("api-version"),
            "capabilities": profile.get("capabilities"),
            "status": evaluated["status"],
            "passCount": len(evaluated["pass"]),
            "failCount": len(evaluated["fail"]),
            "optionalFailCount": len(optional_fail),
            "requirements": rows,
            "blockers": [row["requirement"] for row in fails],
            "optionalFails": [leaf(item["path"]) for item in optional_fail],
        }
    baseline = profiles["VP_DXVK_d3d9_baseline"]
    return {
        "dxvkVersion": version,
        "tag": provenance["tag"],
        "repository": "https://github.com/doitsujin/dxvk",
        "sourceKind": "official-profile",
        "sourceFile": str(PROFILES[version].relative_to(ROOT)),
        "sourceProfile": "VP_DXVK_d3d9_baseline",
        "source": provenance["url"],
        "provenance": provenance,
        "status": baseline["status"],
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "stockDxvkSmoke": "BLOCKED",
        "passCount": baseline["passCount"],
        "failCount": baseline["failCount"],
        "optionalFailCount": profiles["VP_DXVK_d3d9_optimal"]["optionalFailCount"],
        "profiles": profiles,
        "blockers": baseline["blockers"],
        "smokeReason": (
            f"Stock DXVK {provenance['tag']} D3D9 baseline requires "
            + ", ".join(baseline["blockers"])
            + ". G615 reports those features false, so vkCreateDevice with the "
            "stock feature set is illegal. No stock DXVK binary was launched."
        ),
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
        "# P13 DXVK D3D9 Re-evaluation",
        "",
        "## Result",
        "",
        f"`{doc['result']}`",
        "",
        "Machine-evaluated D3D9 only. No PanVK patch. No feature-bit change. P14 was not started.",
        "",
        "```text",
        f"DXVK_1_10_3_D3D9={doc['versions']['1.10.3']['status']}",
        f"DXVK_1_10_3_SMOKE={doc['versions']['1.10.3']['stockDxvkSmoke']}",
        f"DXVK_2_7_1_D3D9={doc['versions']['2.7.1']['status']}",
        f"DXVK_2_7_1_SMOKE={doc['versions']['2.7.1']['stockDxvkSmoke']}",
        f"DXVK_3_1_1_D3D9={doc['versions']['3.1.1']['status']}",
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
        "required D3D9 Vulkan features remain `false`. `vkCreateDevice` with those bits is",
        "`VK_ERROR_FEATURE_NOT_PRESENT`. No game boot, no DXVK init log, no reported D3D",
        "feature level. Smoke is `BLOCKED` for all three versions.",
        "",
        "P9-P12 left `textureCompressionBC`, `shaderClipDistance`, `shaderCullDistance`,",
        "`fillModeNonSolid`, and `geometryShader` false. P13 did not flip them.",
        "",
    ]
    v110 = doc["versions"]["1.10.3"]
    lines += [
        "## DXVK 1.10.3 D3D9",
        "",
        f"Status: **{v110['status']}**. Smoke: **{v110['stockDxvkSmoke']}**.",
        "",
        f"Source: `{v110['source']}` (`{v110['sourceFile']}`).",
        "",
        v110["smokeReason"],
        "",
        "### Required failures",
        "",
        md_table(v110["requirements"], failures_only=True),
        "### All required D3D9 features",
        "",
        md_table(v110["requirements"]),
    ]
    for version in ("2.7.1", "3.1.1"):
        item = doc["versions"][version]
        baseline = item["profiles"]["VP_DXVK_d3d9_baseline"]
        optimal = item["profiles"]["VP_DXVK_d3d9_optimal"]
        lines += [
            f"## DXVK {version} D3D9",
            "",
            f"Status: **{item['status']}**. Smoke: **{item['stockDxvkSmoke']}**.",
            "",
            f"Source: `{item['source']}` (`{item['sourceFile']}`, `{item['provenance']['originalSha256']}`).",
            "",
            item["smokeReason"],
            "",
            f"### VP_DXVK_d3d9_baseline: {baseline['status']}",
            "",
            f"Pass `{baseline['passCount']}` / fail `{baseline['failCount']}`. Optional does not apply.",
            "",
            md_table(baseline["requirements"], failures_only=True),
            f"### VP_DXVK_d3d9_optimal: {optimal['status']}",
            "",
            f"Required pass `{optimal['passCount']}` / fail `{optimal['failCount']}`. Optional fail `{optimal['optionalFailCount']}` (does not fail the profile).",
            "",
            "Required failures match baseline. Optional failures are recorded in `p13-d3d9.json` and are not D3D9 blockers.",
            "",
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
        "| `multiViewport` | P14 | Not started; required only by DXVK 1.10.3 D3D9 source, not by VP_DXVK D3D9 baseline |",
        "",
        "## Checks",
        "",
        "- `scripts/evaluate-vulkan-profile.py` via `evaluate-consumer-profile.py` against `consumer-capabilities.json`.",
        "- Source-derived `validation/requirements/dxvk-1.10.3.json` D3D9 tier.",
        "- Official `validation/g615-v11-csf/profiles/dxvk-2.7.1.json` and `dxvk-3.1.1.json`.",
        "- `tests/dxvk-vkd3d/test_p13_d3d9_reevaluation.py`.",
        "",
        "## Exit",
        "",
        "P13 exits `FAIL`. All three D3D9 evaluations fail. Stock DXVK smoke is `BLOCKED`.",
        "Next engineering step remains P9-P12 implementations, then P14 `multiViewport` for 1.10.3 D3D9.",
        "P14 was not started.",
        "",
    ]
    return "\n".join(lines)


def render_txt(doc):
    lines = [
        "P13 DXVK D3D9 re-evaluation",
        f"result={doc['result']}",
        f"adapter={doc['driver']['adapter']}",
        f"driver={doc['driver']['driverName']} {doc['driver']['driverInfo']}",
        f"api={doc['driver']['apiVersionText']}",
        f"icd={doc['capture']['icdSha256']}",
    ]
    for version, item in doc["versions"].items():
        lines.append(
            f"dxvk-{version} D3D9={item['status']} smoke={item['stockDxvkSmoke']} "
            f"fail={item['failCount']} blockers={','.join(item['blockers'])}"
        )
        lines.append(f"  feature_level={item['d3dFeatureLevel']}")
        lines.append(f"  {item['smokeReason']}")
    lines.append("P13_EXIT=FAIL")
    return "\n".join(lines) + "\n"


def main():
    evaluator = load_evaluator()
    caps = json.loads(CAPS_PATH.read_text())
    versions = {
        "1.10.3": evaluate_legacy(caps, evaluator),
        "2.7.1": evaluate_official(caps, evaluator, "2.7.1"),
        "3.1.1": evaluate_official(caps, evaluator, "3.1.1"),
    }
    doc = {
        "schemaVersion": 1,
        "phase": "P13",
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
        },
        "versions": versions,
    }
    JSON_OUT.write_text(json.dumps(doc, indent=2) + "\n")
    MD_OUT.write_text(render_md(doc))
    TXT_OUT.write_text(render_txt(doc))
    print(f"PASS output={JSON_OUT}")
    print(f"DXVK_1_10_3_D3D9={versions['1.10.3']['status']} SMOKE={versions['1.10.3']['stockDxvkSmoke']}")
    print(f"DXVK_2_7_1_D3D9={versions['2.7.1']['status']} SMOKE={versions['2.7.1']['stockDxvkSmoke']}")
    print(f"DXVK_3_1_1_D3D9={versions['3.1.1']['status']} SMOKE={versions['3.1.1']['stockDxvkSmoke']}")
    print("P13_EXIT=FAIL")


if __name__ == "__main__":
    main()
