#!/usr/bin/env python3
"""Plan-name compatibility entry point for the Vulkan Profile evaluator."""
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).with_name("evaluate-consumer-profile.py")), run_name="__main__")
