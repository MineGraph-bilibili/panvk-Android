#!/usr/bin/env python3
"""Plan-name compatibility entry point for the authoritative G615 probe."""
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).with_name("capture-consumer-capabilities.py")), run_name="__main__")
