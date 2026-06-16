#!/usr/bin/env python3
"""
Analyze RenderDoc capture to find skybox rendering issues.
"""

import sys
import os

# Add RenderDoc to path
renderdoc_path = r"e:\Engine\ZEngine\bin\Debug"
os.environ["PATH"] = renderdoc_path + os.pathsep + os.environ["PATH"]
sys.path.insert(0, renderdoc_path)

try:
    import pyrenderdoc
    print("pyrenderdoc loaded successfully")
except ImportError as e:
    print(f"Failed to import pyrenderdoc: {e}")
    print("Trying to load renderdoc.dll directly...")

    # Try to use ctypes to load renderdoc
    import ctypes
    try:
        renderdoc_dll = ctypes.CDLL(os.path.join(renderdoc_path, "renderdoc.dll"))
        print("renderdoc.dll loaded via ctypes")
    except Exception as e2:
        print(f"Failed to load renderdoc.dll: {e2}")
        sys.exit(1)

def analyze_capture(rdc_path):
    """Analyze a RenderDoc capture file."""
    print(f"Analyzing capture: {rdc_path}")

    if not os.path.exists(rdc_path):
        print(f"File not found: {rdc_path}")
        return

    file_size = os.path.getsize(rdc_path)
    print(f"File size: {file_size / 1024 / 1024:.2f} MB")

    # Try to use renderdoccmd to extract info
    import subprocess
    cmd = [os.path.join(renderdoc_path, "renderdoccmd.exe"), "thumb", rdc_path]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(f"Thumbnail info: {result.stdout}")
    if result.stderr:
        print(f"Stderr: {result.stderr}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        rdc_path = sys.argv[1]
    else:
        rdc_path = r"e:\Engine\ZEngine\bin\1.rdc"

    analyze_capture(rdc_path)
