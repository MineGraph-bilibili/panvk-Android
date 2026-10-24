#!/usr/bin/env python3
"""compute-patch-series-id.py — Deterministic SHA-256 hash of patch inputs.

Usage: python3 scripts/compute-patch-series-id.py [--profile g615-v11-csf]
Output format: sha256:<64 hex chars>
"""
import argparse
import hashlib
import json
import pathlib
import sys

def main():
    parser = argparse.ArgumentParser(description="Compute deterministic patch series ID")
    parser.add_argument("--profile", default="g615-v11-csf", help="Device profile name")
    parser.add_argument("--root", default=None, help="Repository root path")
    parser.add_argument("--bare", action="store_true", help="Print only 64 hex chars without sha256: prefix")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve() if args.root else pathlib.Path(__file__).resolve().parent.parent

    # 1. Mesa commit
    lock_file = root / "sources.lock"
    if not lock_file.exists():
        sys.stderr.write(f"Error: {lock_file} not found\n")
        sys.exit(1)

    lock_data = json.loads(lock_file.read_text())
    mesa_commit = lock_data.get("mesaCommit", "").strip()
    if not mesa_commit:
        sys.stderr.write("Error: mesaCommit missing in sources.lock\n")
        sys.exit(1)

    # 2. Profile JSON
    profile_path = root / "profiles" / f"{args.profile}.json"
    if not profile_path.exists():
        sys.stderr.write(f"Error: {profile_path} not found\n")
        sys.exit(1)

    h = hashlib.sha256()

    # Feed Mesa commit
    h.update(f"mesaCommit:{mesa_commit}\n".encode("utf-8"))

    # Feed normalized profile JSON
    profile_obj = json.loads(profile_path.read_text())
    profile_canonical = json.dumps(profile_obj, sort_keys=True, indent=2)
    h.update(f"profile:{profile_path.name}\n".encode("utf-8"))
    h.update(profile_canonical.encode("utf-8"))
    h.update(b"\n")

    # 3. Patch and overlay files sorted deterministically
    patches_dir = root / "patches"
    if patches_dir.exists():
        patch_files = []
        for p in sorted(patches_dir.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(root).as_posix()
            if p.name.startswith(".") or p.name.lower().startswith("readme"):
                continue
            if p.suffix == ".patch" or "/files/" in rel:
                patch_files.append((rel, p))

        patch_files.sort(key=lambda x: x[0])
        for rel_posix, file_path in patch_files:
            h.update(f"file:{rel_posix}\n".encode("utf-8"))
            h.update(file_path.read_bytes())
            h.update(b"\n")

    digest = h.hexdigest()
    if args.bare:
        print(digest)
    else:
        print(f"sha256:{digest}")

if __name__ == "__main__":
    main()
