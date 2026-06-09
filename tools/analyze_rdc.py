#!/usr/bin/env python3
"""
Analyze RenderDoc .rdc capture file to find skybox rendering issues.
"""

import sys
import os

# Add RenderDoc to path
renderdoc_path = r"e:\Engine\ZEngine\bin\RelWithDebInfo"
sys.path.insert(0, renderdoc_path)

try:
    import pyrenderdoc
    print("pyrenderdoc loaded successfully")
except ImportError as e:
    print(f"Failed to import pyrenderdoc: {e}")
    print("Trying alternative import...")
    try:
        # Try loading from renderdoc.dll
        import ctypes
        renderdoc_dll = ctypes.CDLL(os.path.join(renderdoc_path, "renderdoc.dll"))
        print("renderdoc.dll loaded")
    except Exception as e2:
        print(f"Failed to load renderdoc.dll: {e2}")
        sys.exit(1)

def analyze_capture(rdc_path):
    """Analyze a RenderDoc capture file."""
    print(f"Analyzing capture: {rdc_path}")
    
    # TODO: Use RenderDoc API to load and analyze the capture
    # For now, just print basic information
    
    if not os.path.exists(rdc_path):
        print(f"File not found: {rdc_path}")
        return
    
    file_size = os.path.getsize(rdc_path)
    print(f"File size: {file_size / 1024 / 1024:.2f} MB")
    
    # Try to use renderdoccmd to get info
    import subprocess
    cmd = [os.path.join(renderdoc_path, "renderdoccmd.exe"), "version"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    print(f"RenderDoc version: {result.stdout}")

if __name__ == "__main__":
    rdc_path = r"e:\Engine\ZEngine\bin\RelWithDebInfo\1.rdc"
    analyze_capture(rdc_path)
