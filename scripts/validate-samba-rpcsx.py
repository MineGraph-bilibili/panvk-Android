#!/usr/bin/env python3
"""Validate Packet 2 source provenance and Samba/RPCSX evidence."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
ALLOWED = {"PASS", "PARTIAL", "BLOCKED", "UNSUPPORTED", "NOT_TESTED"}
REQUIRED_RUNTIME = {
    "archiveAcceptance", "elfAcceptance", "customSelection", "rpcsxSetCustomDriver",
    "libadrenotoolsLoad", "exactIcdLoaded", "systemMaliAbsent", "g615Enumeration",
    "vkDevice", "androidSurface", "swapchain", "rpcsxVulkanInitialization",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, text=True,
                          capture_output=True).stdout.strip()


def validate(manifest_path):
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schemaVersion") != 1:
        raise ValueError("unsupported manifest schema")
    for name in ("sambaS3", "rpcsx", "libadrenotools"):
        item = manifest["repositories"][name]
        repo = pathlib.Path(item["path"])
        if not re.fullmatch(r"[0-9a-f]{40}", item["commit"]):
            raise ValueError(f"{name}: invalid commit")
        if git(repo, "rev-parse", "HEAD") != item["commit"]:
            raise ValueError(f"{name}: HEAD differs from recorded commit")
        for owned in item["ownedPaths"]:
            if not (repo / owned).is_file():
                raise ValueError(f"{name}: missing owned path {owned}")

    samba = pathlib.Path(manifest["repositories"]["sambaS3"]["path"])
    rpcsx = pathlib.Path(manifest["repositories"]["rpcsx"]["path"])
    helper = (samba / "app/src/main/java/com/zenithblue/sambas3/utils/GpuDriverHelper.kt").read_text()
    selection = (samba / "app/src/main/java/com/zenithblue/sambas3/utils/GpuDriverSelection.kt").read_text()
    screen = (samba / "app/src/standard/java/com/zenithblue/sambas3/ui/drivers/GpuDriversScreen.kt").read_text()
    native = (samba / "app/src/main/cpp/native-lib.cpp").read_text()
    device = (rpcsx / "rpcs3/Emu/RSX/VK/vkutils/device.cpp").read_text()
    required_source = (
        "ZipUtil.unzip" in helper,
        "validateInstalledLibrary" in helper,
        "RPCSX.instance.setCustomDriver" in selection,
        "GpuDriverHelper.installDriver" in screen and "IMPORT ADPKG" in screen,
        "ADRENOTOOLS_DRIVER_CUSTOM" in native,
        'access("/dev/mali0", F_OK)' in native,
        "vkGetPhysicalDeviceFormatProperties" in device,
        "enabled_features.textureCompressionBC = pgpu->features.textureCompressionBC" in device,
    )
    if not all(required_source):
        raise ValueError("custom loading or physical-device BC query path missing")
    if re.search(r"texture_compression_bc\s*=.*PANVK", device):
        raise ValueError("PanVK-name-to-BC inference remains")

    evidence_path = manifest_path.parent / "runtime-evidence.json"
    evidence = json.loads(evidence_path.read_text())
    if set(evidence["results"]) != REQUIRED_RUNTIME:
        raise ValueError("runtime evidence fields mismatch")
    if any(item["status"] not in ALLOWED for item in evidence["results"].values()):
        raise ValueError("invalid runtime status")
    for key in ("sourceCommit", "builtBinarySha256", "target", "timestamp", "command", "status"):
        if key not in evidence:
            raise ValueError(f"runtime evidence missing {key}")
    if evidence["status"] not in ALLOWED:
        raise ValueError("invalid aggregate status")
    expected = evidence["androidIcdSha256"]
    if expected != "576e37de9a3b60dda9a972a6f91c3a50e09238bd31a9888e04215c7b554c803a":
        raise ValueError("unexpected Android PanVK ICD hash")
    artifact = evidence.get("builtBinaryPath")
    if artifact and pathlib.Path(artifact).is_file() and sha256(pathlib.Path(artifact)) != evidence["builtBinarySha256"]:
        raise ValueError("built binary hash mismatch")
    print(f"{evidence['status']} manifest={manifest_path} runtime={evidence_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        validate(args.manifest.resolve())
    except (KeyError, ValueError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"BLOCKED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
