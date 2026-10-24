#!/usr/bin/env python3
"""capture-runtime-features.py — probe the exact on-device ICD, not Mesa Matrix.

Usage:
  python3 scripts/capture-runtime-features.py --profile g615-v11-csf
  python3 scripts/capture-runtime-features.py --parse dump.txt --out matrix.json

Default path: build panvk-feature-dump, adb push, run against
/data/local/tmp/libvulkan_panfrost.so, write:
  dist/runtime-feature-matrix.json
  validation/<profile>/beta2-runtime-features-YYYY-MM-DD.json
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Mesa panvk_vX_physical_device.c / panvk_instance.c for PAN_ARCH >= 11.
# True = driver implements and advertises the path in source. False = hardcoded off.
UPSTREAM = {
    "robustBufferAccess": True,
    "geometryShader": False,
    "tessellationShader": False,
    "multiViewport": False,
    "shaderClipDistance": False,
    "shaderCullDistance": False,
    "shaderFloat64": False,
    "depthBounds": False,
    "wideLines": True,
    "largePoints": True,
    "samplerAnisotropy": True,
    "textureCompressionBC": True,  # Mesa has BC probe path; G615 runtime may still be false
    "textureCompressionETC2": True,
    "textureCompressionASTC_LDR": True,
    "descriptorIndexing": True,
    "timelineSemaphore": True,
    "dynamicRendering": True,
    "synchronization2": True,
    "bufferDeviceAddress": True,
    "VK_KHR_push_descriptor": True,
    "VK_KHR_swapchain": True,
    "VK_KHR_android_surface": True,
    "externalMemory": True,
    "externalSemaphore": True,
    "computeShader": True,
    "vertexShader": True,
    "fragmentShader": True,
    "bufferAllocation": True,
    "ahbImport": True,
    "colorAttachmentRendering": True,
    "binarySemaphore": True,
    "fence": True,
    "swapchainPresentation": True,
}

# Workloads that actually ran on this beta.2 tree. Bits being true is not enough.
TESTED = {
    "computeShader": True,
    "vertexShader": True,
    "fragmentShader": True,
    "bufferAllocation": True,
    "ahbImport": True,
    "colorAttachmentRendering": True,
    "binarySemaphore": True,
    "fence": True,
    "swapchainPresentation": True,
    "robustBufferAccess": False,
    "geometryShader": False,
    "tessellationShader": False,
    "multiViewport": False,
    "shaderClipDistance": False,
    "shaderCullDistance": False,
    "shaderFloat64": False,
    "depthBounds": False,
    "wideLines": False,
    "largePoints": False,
    "samplerAnisotropy": False,
    "textureCompressionBC": False,
    "textureCompressionETC2": False,
    "textureCompressionASTC_LDR": False,
    "descriptorIndexing": False,
    "timelineSemaphore": False,
    "dynamicRendering": False,
    "synchronization2": False,
    "bufferDeviceAddress": False,
    "VK_KHR_push_descriptor": False,
    "VK_KHR_swapchain": True,
    "VK_KHR_android_surface": True,
    "externalMemory": True,
    "externalSemaphore": True,
}

PLAN_FEATURE_KEYS = [
    "robustBufferAccess",
    "geometryShader",
    "tessellationShader",
    "multiViewport",
    "shaderClipDistance",
    "shaderCullDistance",
    "shaderFloat64",
    "depthBounds",
    "wideLines",
    "largePoints",
    "samplerAnisotropy",
    "textureCompressionBC",
    "textureCompressionETC2",
    "textureCompressionASTC_LDR",
    "descriptorIndexing",
    "timelineSemaphore",
    "dynamicRendering",
    "synchronization2",
    "bufferDeviceAddress",
    "VK_KHR_push_descriptor",
    "VK_KHR_swapchain",
    "VK_KHR_android_surface",
    "externalMemory",
    "externalSemaphore",
    "computeShader",
    "vertexShader",
    "fragmentShader",
    "bufferAllocation",
    "ahbImport",
    "colorAttachmentRendering",
    "binarySemaphore",
    "fence",
    "swapchainPresentation",
]

DUMP_BOOL = re.compile(r"^\s+(\S+)\s+:\s+(YES|NO)\s*$")
DUMP_KV = re.compile(r"^\s+(\w+):\s+(.*)\s*$")
EXT_LINE = re.compile(r"^\s+(VK_\S+)\s+\(v(\d+)\)\s*$")
EXT_HDR = re.compile(r"^=== (INSTANCE|DEVICE) EXTENSIONS \((\d+)\) ===$")


def triple(name: str, exposed: bool) -> dict:
    return {
        "upstreamImplemented": bool(UPSTREAM.get(name, exposed)),
        "runtimeExposed": bool(exposed),
        "runtimeTested": bool(TESTED.get(name, False) and exposed),
    }


def parse_dump(text: str) -> dict:
    instance_exts: list[str] = []
    device_exts: list[str] = []
    bits: dict[str, bool] = {}
    props: dict[str, str] = {}
    section = None
    inst_count = dev_count = None
    for ln in text.splitlines():
        m = EXT_HDR.match(ln)
        if m:
            section = "inst" if m.group(1) == "INSTANCE" else "dev"
            n = int(m.group(2))
            if section == "inst":
                inst_count = n
            else:
                dev_count = n
            continue
        if ln.startswith("=== PHYSICAL DEVICE"):
            section = "phys"
            continue
        if ln.startswith("=== VULKAN"):
            section = "feat"
            continue
        em = EXT_LINE.match(ln)
        if em and section in ("inst", "dev"):
            (instance_exts if section == "inst" else device_exts).append(em.group(1))
            continue
        bm = DUMP_BOOL.match(ln)
        if bm and section == "feat":
            bits[bm.group(1)] = bm.group(2) == "YES"
            continue
        km = DUMP_KV.match(ln)
        if km and section == "phys":
            props[km.group(1)] = km.group(2).strip()

    inst_set = set(instance_exts)
    dev_set = set(device_exts)
    exposed = dict(bits)
    exposed["VK_KHR_push_descriptor"] = "VK_KHR_push_descriptor" in dev_set
    exposed["VK_KHR_swapchain"] = "VK_KHR_swapchain" in dev_set
    exposed["VK_KHR_android_surface"] = "VK_KHR_android_surface" in inst_set
    exposed["externalMemory"] = (
        "VK_KHR_external_memory" in dev_set
        or "VK_KHR_external_memory_fd" in dev_set
        or "VK_ANDROID_external_memory_android_hardware_buffer" in dev_set
    )
    exposed["externalSemaphore"] = (
        "VK_KHR_external_semaphore" in dev_set
        or "VK_KHR_external_semaphore_fd" in dev_set
    )
    # Shader stages / allocation are not VkPhysicalDeviceFeatures bits; they
    # are exercised by existing gates (compute/offscreen/AHB/WSI).
    exposed["computeShader"] = True
    exposed["vertexShader"] = True
    exposed["fragmentShader"] = True
    exposed["bufferAllocation"] = True
    exposed["ahbImport"] = "VK_ANDROID_external_memory_android_hardware_buffer" in dev_set
    exposed["colorAttachmentRendering"] = True
    exposed["binarySemaphore"] = True
    exposed["fence"] = True
    exposed["swapchainPresentation"] = exposed["VK_KHR_swapchain"]

    features = {k: triple(k, bool(exposed.get(k, False))) for k in PLAN_FEATURE_KEYS}
    for k, v in bits.items():
        if k not in features:
            features[k] = triple(k, v)

    if inst_count is None:
        inst_count = len(instance_exts)
    if dev_count is None:
        dev_count = len(device_exts)

    return {
        "source": "on-device panvk-feature-dump",
        "deviceName": props.get("deviceName"),
        "vendorID": props.get("vendorID"),
        "deviceID": props.get("deviceID"),
        "driverID": props.get("driverID"),
        "driverName": props.get("driverName"),
        "driverInfo": props.get("driverInfo"),
        "driverVersion": props.get("driverVersion"),
        "apiVersion": props.get("apiVersion"),
        "instanceExtensionCount": inst_count,
        "deviceExtensionCount": dev_count,
        "totalExtensionCount": inst_count + dev_count,
        "instanceExtensions": instance_exts,
        "deviceExtensions": device_exts,
        "features": features,
        "schema": {
            "upstreamImplemented": "Mesa PanVK source advertises/implements the path",
            "runtimeExposed": "queried from the exact ICD on this device",
            "runtimeTested": "a real workload on this tree exercised the path",
        },
    }


def ndk_clang() -> pathlib.Path:
    ndk = os.environ.get("ANDROID_NDK_ROOT") or os.environ.get("ANDROID_NDK")
    if not ndk:
        ndk = "/opt/android-sdk/ndk/30.0.14904198"
    p = pathlib.Path(ndk)
    if (p / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang").exists():
        return p / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"
    hits = list(p.glob("*/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"))
    if hits:
        return hits[-1]
    hits = list(pathlib.Path("/opt/android-sdk/ndk").glob("*/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"))
    if hits:
        return hits[-1]
    raise SystemExit("NDK aarch64-linux-android35-clang not found")


def build_dump() -> pathlib.Path:
    cc = ndk_clang()
    src = ROOT / "tests/vulkan-smoke/feature_dump.c"
    out = ROOT / "tests/vulkan-smoke/panvk-feature-dump"
    subprocess.check_call(
        [str(cc), "-O2", "-fPIE", "-pie", "-fPIC", "-o", str(out), str(src), "-ldl"]
    )
    return out


def adb(serial: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += list(args)
    return subprocess.run(cmd, check=check, text=True, capture_output=True)


def run_on_device(dump: pathlib.Path, serial: str, so_remote: str) -> str:
    adb(serial, "push", str(dump), "/data/local/tmp/panvk-feature-dump")
    adb(serial, "shell", "chmod", "755", "/data/local/tmp/panvk-feature-dump")
    r = adb(serial, "shell", "/data/local/tmp/panvk-feature-dump", so_remote)
    return (r.stdout or "") + (r.stderr or "")


def sha256(path: pathlib.Path) -> str | None:
    if not path.exists():
        return None
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def load_profile(name: str) -> dict:
    p = ROOT / "profiles" / f"{name}.json"
    return json.loads(p.read_text()) if p.exists() else {}


def load_lock() -> dict:
    p = ROOT / "sources.lock"
    return json.loads(p.read_text()) if p.exists() else {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default="g615-v11-csf")
    ap.add_argument("--parse", help="parse existing dump text instead of running on device")
    ap.add_argument("--out", help="write matrix JSON here")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-device", action="store_true")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL", "Y5WWBMJVOZSK4HU8"))
    ap.add_argument("--so", default="/data/local/tmp/libvulkan_panfrost.so")
    a = ap.parse_args()

    if a.parse:
        text = pathlib.Path(a.parse).read_text(errors="replace")
    else:
        dump = ROOT / "tests/vulkan-smoke/panvk-feature-dump"
        if not a.no_build:
            dump = build_dump()
        if a.no_device:
            raise SystemExit("need --parse dump.txt or a device run")
        text = run_on_device(dump, a.serial, a.so)

    matrix = parse_dump(text)
    profile = load_profile(a.profile)
    lock = load_lock()
    so_local = ROOT / "dist" / f"android-{a.profile}" / "libvulkan_panfrost.so"
    matrix.update(
        {
            "profile": a.profile,
            "device": profile.get("device", "Poco X6 Pro"),
            "soc": profile.get("soc"),
            "gpu": profile.get("gpu"),
            "gpuId": profile.get("gpuId"),
            "panArch": profile.get("panArch"),
            "frontend": profile.get("frontend"),
            "kbaseUapi": profile.get("kbaseUapi"),
            "mesaCommit": lock.get("mesaCommit"),
            "mesaVersion": lock.get("mesaVersion"),
            "binarySha256": sha256(so_local),
            "capturedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        }
    )

    date = datetime.date.today().isoformat()
    vdir = ROOT / "validation" / a.profile
    vdir.mkdir(parents=True, exist_ok=True)
    (vdir / f"beta2-runtime-features-{date}.txt").write_text(text if text.endswith("\n") else text + "\n")
    default_val = vdir / f"beta2-runtime-features-{date}.json"
    default_dist = ROOT / "dist" / "runtime-feature-matrix.json"
    out = pathlib.Path(a.out) if a.out else default_dist
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(matrix, indent=2) + "\n"
    out.write_text(payload)
    default_val.write_text(payload)
    if out.resolve() != default_dist.resolve():
        default_dist.write_text(payload)

    n_exp = sum(1 for v in matrix["features"].values() if v.get("runtimeExposed"))
    n_test = sum(1 for v in matrix["features"].values() if v.get("runtimeTested"))
    print(
        f"OK inst={matrix['instanceExtensionCount']} "
        f"dev={matrix['deviceExtensionCount']} "
        f"exposed={n_exp} tested={n_test} out={out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
