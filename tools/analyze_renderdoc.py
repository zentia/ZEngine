#!/usr/bin/env python3
"""
Analyze RenderDoc .rdc capture file to debug skybox rendering.
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
    print("\nTrying to load renderdoc.dll directly...")
    try:
        import ctypes
        renderdoc_dll = ctypes.CDLL(os.path.join(renderdoc_path, "renderdoc.dll"))
        print("renderdoc.dll loaded successfully")
        
        # Try to get pyrenderdoc module
        # This is a simplified approach - actual RenderDoc Python API usage may differ
    except Exception as e2:
        print(f"Failed to load renderdoc.dll: {e2}")
        sys.exit(1)

def analyze_capture(rdc_path):
    """Analyze a RenderDoc capture file."""
    print(f"\nAnalyzing capture: {rdc_path}")
    
    if not os.path.exists(rdc_path):
        print(f"Error: File not found: {rdc_path}")
        return
    
    file_size = os.path.getsize(rdc_path)
    print(f"File size: {file_size / 1024 / 1024:.2f} MB")
    
    # Try to use renderdoccmd to get info about the capture
    import subprocess
    
    # Get API info
    print("\n=== Attempting to replay capture ===")
    cmd = [os.path.join(renderdoc_path, "renderdoccmd.exe"), "replay", rdc_path]
    print(f"Command: {' '.join(cmd)}")
    print("Note: This requires a display. If running headless, this may fail.")
    
    # Instead, let's try to extract useful information
    print("\n=== RenderDoc Version ===")
    version_cmd = [os.path.join(renderdoc_path, "renderdoccmd.exe"), "version"]
    result = subprocess.run(version_cmd, capture_output=True, text=True, timeout=5)
    print(result.stdout)
    
    print("\n=== Suggestions ===")
    print("1. Open the .rdc file in RenderDoc GUI to see the full render pipeline")
    print("2. Look for draw calls that should render the skybox")
    print("3. Check if sky_mesh.frag.hlsl is being used")
    print("4. Check the render targets after the Sky Pass")

if __name__ == "__main__":
    rdc_path = r"e:\Engine\ZEngine\bin\RelWithDebInfo\1.rdc"
    analyze_capture(rdc_path)
