#!/usr/bin/env python3
"""Generate the P0-P4 G615 gap report from exact imported profile evaluations."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPS = ROOT / "validation/g615-v11-csf/consumer-capabilities.json"
PROFILES = ROOT / "validation/g615-v11-csf/profiles"
OUTPUT = ROOT / "validation/g615-v11-csf/dxvk-vkd3d-gap-report.md"
ABSENT_UPSTREAM = {
    "geometryShader", "tessellationShader", "fillModeNonSolid", "multiViewport",
    "shaderClipDistance", "shaderCullDistance", "pipelineStatisticsQuery",
    "transformFeedback", "geometryStreams",
}

def classify(item):
    leaf = item["path"].rsplit(".", 1)[-1]
    actual = item["actual"]
    if leaf in ABSENT_UPSTREAM:
        return "ABSENT at pinned and origin/main", "NOT_IMPLEMENTED", "Design/implement after P5 gates; never expose early"
    if leaf == "textureCompressionBC":
        return "Hardware-dependent; G615 mask is zero", "UNSUPPORTED_NATIVE", "Keep false unless transparent BC emulation is complete"
    if leaf.startswith("sparse") or "SPARSE_BINDING" in str(item["required"]):
        return "Generic PanVK exists; custom Kbase equivalence unproven", "BLOCKED_KBASE", "Defer sparse; assess Kbase VM semantics only when required"
    if actual is None:
        return "UNKNOWN", "UNKNOWN", "Extend capture or implementation audit before claiming support"
    return "Pinned runtime does not satisfy; no post-pin matching backport found", "NOT_SATISFIED", "Investigate in the owning feature phase"

def main():
    caps = json.loads(CAPS.read_text())
    extensions = {item["name"] for item in caps["extensions"]}
    lines = [
        "# G615 DXVK/vkd3d Gap Report", "",
        "Generated from the direct-ICD capture and exact tagged official profiles. `UNKNOWN` is not failure proof or support proof.", "",
        "## Provenance", "",
        f"- Device: `{caps['device']['name']}` / `0x{caps['device']['deviceID']:08x}`",
        f"- Capture ICD SHA-256: `{caps['capture']['icdSha256']}`",
        "- Pinned Mesa: `5a07217f034b3e50d8c7c7794f97a2df1742613b`",
        "- Audited Mesa origin/main: `e1f3f372c4a661cd0e71a0cdafc4d469d56ecf35`",
        "- Post-pin coherent requirement backports: none", "",
    ]
    legacy = json.loads((ROOT / "validation/requirements/dxvk-1.10.3.json").read_text())
    lines += ["## dxvk-1.10.3 source-derived", ""]
    inherited = []
    for name, tier in legacy["tiers"].items():
        required = inherited + tier.get("requiredFeatures", [])
        failures = [(feature, caps["features"].get(feature)) for feature in required if caps["features"].get(feature) is not True]
        failures += [(extension, extension in extensions) for extension in tier.get("requiredExtensions", []) if extension not in extensions]
        lines += [f"### {name}: {'PASS' if not failures else 'FAIL'}", "", f"Source: `{legacy['tag']}` `{tier['source']}`", ""]
        if failures:
            lines += ["| Requirement | Current | Required | Upstream PanVK | Implementation | Action |", "|---|---:|---:|---|---|---|"]
            for feature, actual in failures:
                upstream, status, action = classify({"path": feature, "actual": actual, "required": True})
                lines.append(f"| `{feature}` | `{actual}` | `True` | {upstream} | {status} | {action} |")
            lines.append("")
        inherited = required
    vkd3d2 = json.loads((ROOT / "validation/requirements/vkd3d-proton-2.0.json").read_text())
    hard = vkd3d2["hardRequirements"]
    limits = [caps["properties"].get(name) for name in ("maxDescriptorSetUpdateAfterBindStorageBuffers", "maxDescriptorSetUpdateAfterBindSampledImages", "maxDescriptorSetUpdateAfterBindStorageImages", "maxDescriptorSetUpdateAfterBindSamplers")]
    indexing = caps["featureStructures"].get("VkPhysicalDeviceDescriptorIndexingFeatures", {})
    excluded = {"descriptorBindingUniformBufferUpdateAfterBind"}
    indexing_ok = all(value for name, value in indexing.items() if name not in excluded)
    checks = {
        "apiVersion >= 1.1": caps["device"]["apiVersion"] >= (1 << 22 | 1 << 12),
        "VK_EXT_descriptor_indexing": "VK_EXT_descriptor_indexing" in extensions,
        "VK_KHR_timeline_semaphore": "VK_KHR_timeline_semaphore" in extensions,
        "required descriptor-indexing features": indexing_ok,
        "required UpdateAfterBind limits >= 1000000": all(value is not None and value >= hard["updateAfterBindMinimum"] for value in limits),
    }
    lines += ["## vkd3d-proton-2.0 source-derived", "", f"Source: `{vkd3d2['tag']}` `{hard['source']}`", "", f"Hard requirements: **{'PASS (reported values only; P5 workload proof pending)' if all(checks.values()) else 'FAIL'}**", "", "| Requirement | Current | Required |", "|---|---:|---:|"]
    lines += [f"| `{name}` | `{actual}` | `True` |" for name, actual in checks.items()]
    lines.append("")
    for path in sorted(PROFILES.glob("*.json")):
        imported = json.loads(path.read_text())
        provenance = imported["provenance"]
        lines += [f"## {path.stem}", "", f"Source: `{provenance['tag']}` `{provenance['url']}` (`{provenance['originalSha256']}`)", ""]
        for name, result in imported["evaluation"]["profiles"].items():
            lines += [f"### {name}: {result['status']}", ""]
            if not result["fail"]:
                lines += ["No required failures.", ""]
                continue
            lines += ["| Requirement | Current | Required | Upstream PanVK | Implementation | Action |", "|---|---:|---:|---|---|---|"]
            for item in result["fail"]:
                upstream, status, action = classify(item)
                lines.append(f"| `{item['path']}` | `{item['actual']}` | `{item['required']}` | {upstream} | {status} | {action} |")
            lines.append("")
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"PASS output={OUTPUT}")

if __name__ == "__main__":
    main()
