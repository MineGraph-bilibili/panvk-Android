#!/bin/sh
# validate-package.sh — canonical ZIP layout + beta+ provenance/gate checks
# Usage: ./scripts/validate-package.sh <package.zip>
set -eu
Z="$1"; [ -f "$Z" ] || { echo "missing $Z" >&2; exit 1; }
unzip -l "$Z"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$Z" "$ROOT" <<'EOF'
import hashlib, json, os, re, sys, zipfile

zpath, root = sys.argv[1], sys.argv[2]
PINNED_MESA = "5a07217f034b3e50d8c7c7794f97a2df1742613b"

def die(msg):
    print(f"PACKAGE-VALIDATION: FAIL {msg}", file=sys.stderr)
    sys.exit(1)

z = zipfile.ZipFile(zpath)
names = set(z.namelist())

def loadj(name):
    if name not in names:
        die(f"missing {name}")
    return json.loads(z.read(name))

if "libvulkan_panfrost.so" not in names:
    die("missing libvulkan_panfrost.so")
if "meta.json" not in names:
    die("missing meta.json")
if "MANIFEST.json" not in names:
    die("missing MANIFEST.json")

meta = loadj("meta.json")
manifest = loadj("MANIFEST.json")
gap = loadj("extension-gap.json")
for k in ("schemaVersion", "name", "libraryName", "abi", "backend", "profile", "sourceCommit"):
    if k not in meta:
        die(f"meta.json missing {k}")
if meta["libraryName"] != "libvulkan_panfrost.so":
    die("canonical binary renamed")
if meta.get("prerelease") is not True or not isinstance(meta.get("published"), bool):
    die("package must declare prerelease=true and a boolean published state")
if meta.get("bcCompatibility", {}).get("included") is not False:
    die("unproven BC compatibility must not be included")
if gap.get("summary", gap.get("_meta", {})).get("runtimeExposed") != 194:
    die("extension-gap.json is not the 194-extension beta.3 candidate report")

abi = meta["abi"]
if abi not in ("android-aarch64-bionic", "linux-aarch64-glibc"):
    die(f"unexpected abi {abi}")

pkg_ver = str(meta.get("packageVersion", ""))
blob = (pkg_ver + " " + os.path.basename(zpath)).lower()

def tier_of(s):
    if re.search(r"(^|[^a-z])dev([^a-z]|$)|-dev", s):
        return "dev"
    if "alpha" in s:
        return "alpha"
    if "beta" in s:
        return "beta"
    if re.search(r"(^|[^a-z])rc([^a-z]|$)|-rc", s):
        return "rc"
    return "stable"

tier = tier_of(blob)
beta_plus = tier in ("beta", "rc", "stable")

android = abi == "android-aarch64-bionic"
required = [
    "libvulkan_panfrost.so",
    "meta.json",
    "MANIFEST.json",
    "SOURCE.json",
    "VALIDATION.json",
    "SHA256SUMS.txt",
    "extension-gap.json",
]
if android:
    required.append("runtime-feature-matrix.json")
else:
    required.append("panfrost_icd.aarch64.json")

if beta_plus:
    for r in required:
        if r not in names:
            if r == "runtime-feature-matrix.json" and not android:
                continue
            die(f"beta+ missing {r}")
    if not android:
        has_matrix = "runtime-feature-matrix.json" in names
        ref = (
            meta.get("runtimeFeatureMatrix")
            or meta.get("runtimeFeatureMatrixRef")
            or manifest.get("runtimeFeatureMatrix")
            or manifest.get("runtimeFeatureMatrixRef")
        )
        if not has_matrix and not ref:
            source = loadj("SOURCE.json") if "SOURCE.json" in names else {}
            ref = source.get("runtimeFeatureMatrix") or source.get("runtimeFeatureMatrixRef")
        if not has_matrix and not ref:
            die("glibc beta+ needs runtime-feature-matrix.json or referenced shared manifest")
        if isinstance(ref, str) and ref not in names and not has_matrix:
            die(f"glibc runtimeFeatureMatrixRef {ref} not in package")

lock_path = os.path.join(root, "sources.lock")
lock = json.loads(open(lock_path).read()) if os.path.isfile(lock_path) else {}
pinned = lock.get("mesaCommit") or PINNED_MESA

src_commit = meta.get("sourceCommit")
if src_commit != pinned:
    die(f"sourceCommit {src_commit} != pinned Mesa {pinned}")

psid = str(meta.get("patchSeriesId", ""))
if not psid or psid.upper().startswith("PENDING") or "PENDING" in psid.upper():
    die(f"patchSeriesId placeholder: {psid!r}")
if psid in ("unknown", "PENDING-P3-REBASE"):
    die(f"patchSeriesId invalid: {psid!r}")

if "SHA256SUMS.txt" in names:
    sums = z.read("SHA256SUMS.txt").decode()
    so = z.read("libvulkan_panfrost.so")
    actual = hashlib.sha256(so).hexdigest()
    listed = None
    for line in sums.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[-1].endswith("libvulkan_panfrost.so"):
            listed = parts[0]
            break
        if len(parts) >= 1 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            listed = parts[0]
            break
    if listed is None:
        die("SHA256SUMS.txt has no libvulkan_panfrost.so digest")
    if listed.lower() != actual:
        die(f"binary SHA mismatch listed={listed} actual={actual}")
    man_sha = str(manifest.get("binarySha256", "")).lower()
    if man_sha and man_sha not in ("missing",) and man_sha != actual:
        die(f"MANIFEST.binarySha256 {man_sha} != actual {actual}")

if manifest.get("abi") and manifest["abi"] != abi:
    die(f"MANIFEST.abi {manifest['abi']} != meta.abi {abi}")
if manifest.get("profile") and manifest["profile"] != meta["profile"]:
    die(f"MANIFEST.profile {manifest['profile']} != meta.profile {meta['profile']}")

prof = meta["profile"]
prof_path = os.path.join(root, "profiles", f"{prof}.json")
if not os.path.isfile(prof_path):
    die(f"unknown profile {prof}")

if android:
    min_api = meta.get("minApi")
    if min_api is None:
        die("android meta.json missing minApi")
    try:
        min_api = int(min_api)
    except (TypeError, ValueError):
        die(f"minApi not int: {min_api!r}")
    # P16: do not advertise API 29 without a proven rebuild. Conservative floor is 35.
    if min_api < 35 and not meta.get("minApiProven"):
        die(f"minApi={min_api} unproven; set minApi=35 or minApiProven=true after API {min_api} rebuild")

if "SOURCE.json" in names:
    source = loadj("SOURCE.json")
    sc = source.get("mesaCommit") or source.get("sourceCommit")
    if sc and sc != pinned:
        die(f"SOURCE.json mesaCommit {sc} != pinned {pinned}")
    spsid = source.get("patchSeriesId")
    if spsid and (str(spsid).upper().startswith("PENDING") or "PENDING" in str(spsid).upper()):
        die(f"SOURCE.json patchSeriesId placeholder: {spsid!r}")

MANDATORY_ANDROID = (
    "enumeration",
    "deviceCreate",
    "compute10",
    "offscreen",
    "ahb",
    "vulkanSurface",
    "swapchain300",
    "coldSecondLaunch",
)
MANDATORY_GLIBC = ("enumeration", "compute10", "offscreen")
FLAT_ALIASES = {
    "vulkanSurface": ("androidSurface300", "vulkanSurface"),
    "swapchain300": ("swapchain300", "androidSurface300"),
    "coldSecondLaunch": ("coldSecondLaunch", "secondRun"),
    "enumeration": ("enumeration", "glibcVulkaninfo"),
    "compute10": ("compute10", "glibcCompute"),
    "offscreen": ("offscreen", "glibcOffscreen"),
}

def status_of(valid, key, section):
    if isinstance(valid.get(section), dict) and key in valid[section]:
        return valid[section][key]
    if key in valid:
        return valid[key]
    for alt in FLAT_ALIASES.get(key, ()):
        if alt in valid:
            return valid[alt]
        if isinstance(valid.get(section), dict) and alt in valid[section]:
            return valid[section][alt]
    android_map = valid.get("android") if isinstance(valid.get("android"), dict) else {}
    glibc_map = valid.get("glibc") if isinstance(valid.get("glibc"), dict) else {}
    if key in android_map:
        return android_map[key]
    if key in glibc_map:
        return glibc_map[key]
    return None

def unwrap(v):
    if isinstance(v, dict):
        return str(v.get("result") or v.get("status") or v.get("state") or "untested")
    return str(v if v is not None else "untested")

if beta_plus and "VALIDATION.json" in names:
    valid = loadj("VALIDATION.json")
    keys = MANDATORY_ANDROID if android else MANDATORY_GLIBC
    section = "android" if android else "glibc"
    untested = []
    failed = []
    for k in keys:
        st = unwrap(status_of(valid, k, section)).lower()
        if st in ("untested", "", "none", "null"):
            untested.append(k)
        elif st not in ("pass", "passed", "ok", "true"):
            failed.append(f"{k}={st}")
    if untested:
        die(f"mandatory {section} gates untested: {','.join(untested)}")
    if failed:
        die(f"mandatory {section} gates not pass: {','.join(failed)}")

print("PACKAGE-VALIDATION: PASS", abi, meta["profile"], f"tier={tier}")
EOF
