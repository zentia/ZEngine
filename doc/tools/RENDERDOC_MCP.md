# RenderDoc MCP (Python / scheme B)

Lets Cursor agents open `.rdc` captures and query viewport, pipeline state, mesh
output, and pixel history without manual screenshots.

## One-time setup

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_renderdoc_mcp.ps1
```

This script:

1. Clones [Linkingooo/renderdoc-mcp](https://github.com/Linkingooo/renderdoc-mcp) into `tools/renderdoc-mcp/`
2. Runs `pip install -e .` (Python 3.10+)
3. Finds `renderdoc.pyd` and writes `.cursor/mcp.json`

**Prerequisite:** `renderdoc.pyd` must exist and must match your active Python (3.10+).
The setup script auto-rebuilds `pyrenderdoc_module` when import fails.

Typical locations:

| Source | Path |
|--------|------|
| Vendored build | `tools/renderdoc/x64/Development/pymodules/renderdoc.pyd` |
| Official install | `C:\Program Files\RenderDoc\renderdoc.pyd` |

`renderdoc.dll` lives one directory up (`x64/Development/`). MCP config sets
`RENDERDOC_DLL_PATH` for that folder.

Force rebuild against the Python on PATH:

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_renderdoc_mcp.ps1 -RebuildPythonModule
```

Override search with env var `RENDERDOC_MODULE_PATH` (directory containing `renderdoc.pyd`).

Restart Cursor after setup so the **renderdoc** MCP server loads.

## Capture workflow with ZEditor

1. Launch ZEditor with RenderDoc loaded (default when `renderdoc.dll` is present).
2. Select an object in the Scene (axis gizmo diagnostics require selection).
3. Press **Ctrl+Shift+F12** to capture.
4. Console logs:
   - `RenderDoc: capture template set to .../Captures/zeditor`
   - `RenderDoc: capture saved to E:/Engine/ZEngine/Captures/zeditor_frameXXXX.rdc`
5. Paste the `.rdc` path into chat; the agent can open it via MCP.

Captures land under `<repo>/Captures/` (gitignored).

## Engine-side axis diagnostics

When an axis gizmo is drawn, ZRender logs once per axis mesh id:

```
DrawAxis diag: indices=... swapchain=... scene_vp=(...) draw_vp=(...)
origin_ndc=(...) origin_px=(...) inside_scene=0|1
```

- `inside_scene=0` means the gizmo projects outside the Scene panel rect (viewport mismatch).
- `inside_scene=1` but still invisible: use MCP pixel history on `origin_px`.

## MCP client config (manual)

If you prefer to edit by hand, `.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "renderdoc": {
      "command": "python",
      "args": ["-m", "renderdoc_mcp"],
      "env": {
        "RENDERDOC_MODULE_PATH": "E:/Engine/ZEngine/tools/renderdoc/x64/Development/pymodules",
        "RENDERDOC_DLL_PATH": "E:/Engine/ZEngine/tools/renderdoc/x64/Development"
      }
    }
  }
}
```

Use forward slashes in paths on Windows.
