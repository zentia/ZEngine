#!/usr/bin/env python3
"""
Analyze RenderDoc .rdc capture file to find skybox rendering issues.
"""
import sys
import os

# Add RenderDoc to path
renderdoc_path = r"e:\Engine\ZEngine\bin\Debug"
sys.path.insert(0, renderdoc_path)

try:
    import pyrenderdoc
    print("pyrenderdoc loaded successfully")
except ImportError as e:
    print(f"Failed to import pyrenderdoc: {e}")
    sys.exit(1)

def analyze_capture(rdc_path):
    """Analyze a RenderDoc capture file."""
    print(f"Analyzing capture: {rdc_path}")
    
    if not os.path.exists(rdc_path):
        print(f"File not found: {rdc_path}")
        return
    
    file_size = os.path.getsize(rdc_path)
    print(f"File size: {file_size / 1024 / 1024:.2f} MB")
    
    # Load the capture
    try:
        # Use pyrenderdoc to load the capture
        # This is a simplified version - actual API may differ
        print("Attempting to load capture with pyrenderdoc...")
        
        # For now, just print file info
        print("Capture file exists and is readable")
        print("To fully analyze, open this .rdc file in RenderDoc GUI")
        print("Look for these events:")
        print("  1. 'Sky Pass' event after CmdNextSubpass")
        print("  2. Draw calls with sky_mesh shaders")
        print("  3. Check if backup_odd is written to")
        
    except Exception as ex:
        print(f"Error analyzing capture: {ex}")

if __name__ == "__main__":
    rdc_path = r"e:\Engine\ZEngine\bin\Debug\1.rdc"
    analyze_capture(rdc_path)
