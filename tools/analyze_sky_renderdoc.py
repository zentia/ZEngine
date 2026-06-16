#!/usr/bin/env python3
"""
Analyze RenderDoc capture to debug sky rendering issue.
Checks if the sky draw call is executing and writing to the correct render target.
"""

import sys
import os

def analyze_renderdoc_capture(rdc_path):
    """
    Analyze a RenderDoc capture file.
    This is a placeholder - actual implementation would use RenderDoc's Python API.
    """
    if not os.path.exists(rdc_path):
        print(f"ERROR: Capture file not found: {rdc_path}")
        return False
    
    print(f"Analyzing RenderDoc capture: {rdc_path}")
    print("=" * 80)
    
    # NOTE: This script requires RenderDoc's Python module (pyrenderdoc)
    # To use this script:
    # 1. Install RenderDoc Python module: pip install pyrenderdoc
    # 2. Or use RenderDoc's built-in Python console
    
    print("\nThis script requires RenderDoc's Python API (pyrenderdoc).")
    print("To analyze the capture:")
    print("1. Open the capture in RenderDoc GUI")
    print("2. Find the 'CmdDrawIndexed' call with index_count=3072")
    print("3. Check:")
    print("   - Pipeline state: depth test = ALWAYS?")
    print("   - Vertex shader output: are vertices in clip space?")
    print("   - Pixel shader: is it outputting red (1, 0, 0, 1)?")
    print("   - Render target: is BackupOdd red after the draw call?")
    print("\nAlternatively, use RenderDoc's Python console:")
    print("  - Open RenderDoc -> Tools -> Python Shell")
    print("  - Load the capture and inspect the sky draw call")
    
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_sky_renderdoc.py <capture.rdc>")
        print("\nExample:")
        print("  python analyze_sky_renderdoc.py bin/1.rdc")
        return 1
    
    rdc_path = sys.argv[1]
    analyze_renderdoc_capture(rdc_path)
    return 0

if __name__ == "__main__":
    sys.exit(main())
