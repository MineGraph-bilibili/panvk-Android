#!/usr/bin/env python3
"""Import an exact tagged official vkd3d-proton Vulkan Profile."""
import argparse
import runpy
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("version", choices=("2.14.1", "3.0.1"))
parser.add_argument("--output")
args = parser.parse_args()
sys.argv = [sys.argv[0], "--url", f"https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/v{args.version}/VP_D3D12_VKD3D_PROTON_profile.json", "--tag", f"v{args.version}", "--output", args.output or f"validation/g615-v11-csf/profiles/vkd3d-{args.version}.json"]
runpy.run_path(str(Path(__file__).with_name("import-consumer-profile.py")), run_name="__main__")
