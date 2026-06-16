#!/usr/bin/env python3
"""
Analyze RenderDoc .rdc capture file to find skybox rendering issues.
Uses RenderDoc's Python API to load and analyze the capture.
"""

import sys
import os

# The renderdoc module is provided by RenderDoc's installation
# Try to import it
try:
    import renderdoc as rd
    print("renderdoc module loaded successfully")
except ImportError:
    print("ERROR: Cannot import renderdoc module")
    print("Make sure RenderDoc is installed and the Python module is in sys.path")
    print("The renderdoc Python module is usually installed to:")
    print("  - C:\\Program Files\\RenderDoc\\pymodules\\")
    print("  - Or the directory containing renderdoc.dll")
    sys.exit(1)

def analyze_capture(rdc_path):
    """Load and analyze a RenderDoc capture file."""

    if not os.path.exists(rdc_path):
        print(f"ERROR: File not found: {rdc_path}")
        return

    print(f"Loading capture: {rdc_path}")

    # Load the capture
    try:
        cap = rd.OpenCaptureFile(rdc_path)
        if not cap.LocalReplaySupport():
            print("ERROR: Capture does not support local replay")
            return

        # Create a replay controller
        status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
        if status != rd.StatusCode.Succeeded:
            print(f"ERROR: Failed to open capture: {status}")
            return

        print("Capture loaded successfully")

        # Get the draw call list
        draw_calls = controller.GetDrawCalls()

        # Find skybox-related draw calls
        skybox_draws = []
        all_draws = []

        def enumerate_draws(draw_list, depth=0):
            for draw in draw_list:
                all_draws.append((depth, draw))
                if draw.name and ("sky" in draw.name.lower() or "mesh" in draw.name.lower()):
                    skybox_draws.append((depth, draw))
                if draw.children:
                    enumerate_draws(draw.children, depth + 1)

        enumerate_draws(draw_calls)

        print(f"\nTotal draw calls: {len(all_draws)}")
        print(f"Potential skybox draw calls: {len(skybox_draws)}")

        if skybox_draws:
            print("\nSkybox draw calls:")
            for depth, draw in skybox_draws:
                print(f"  {'  ' * depth}{draw.name} (event_id={draw.event_id})")
        else:
            print("\nNo skybox draw calls found by name.")
            print("Recent draw calls:")
            for depth, draw in all_draws[-20:]:
                print(f"  {'  ' * depth}{draw.name} (event_id={draw.event_id})")

        # Close the capture
        controller.Shutdown()
        cap.Shutdown()

    except Exception as e:
        print(f"ERROR: Exception while analyzing capture: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    if len(sys.argv) > 1:
        rdc_path = sys.argv[1]
    else:
        rdc_path = r"e:\Engine\ZEngine\bin\1.rdc"

    analyze_capture(rdc_path)
