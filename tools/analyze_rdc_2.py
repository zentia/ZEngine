import os
import sys

sys.path.insert(0, r"E:\Engine\ZEngine\tools\renderdoc\x64\Development\pymodules")
os.add_dll_directory(r"E:\Engine\ZEngine\tools\renderdoc\x64\Development")
import renderdoc as rd


def res_name(ctrl, rid):
    for r in ctrl.GetResources():
        if int(r.resourceId) == int(rid):
            return r.name
    return str(rid)


def flatten(actions, out=None):
    if out is None:
        out = []
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def mesh_ndc(ctrl):
    fmt = ctrl.GetPostVSData(rd.MeshDataStage.VSOut, 0, 0)
    if not fmt:
        return None
    try:
        mesh = ctrl.GetMeshData(fmt, 0, 0)
    except Exception as exc:
        return {"error": str(exc)}
    if not mesh or not mesh.position:
        return None
    xs, ys, zs = [], [], []
    for p in mesh.position:
        xs.append(p.x)
        ys.append(p.y)
        zs.append(p.z)
    return {
        "count": len(mesh.position),
        "x": (min(xs), max(xs)),
        "y": (min(ys), max(ys)),
        "z": (min(zs), max(zs)),
    }


def main():
    cap_path = sys.argv[1] if len(sys.argv) > 1 else r"E:\Engine\ZEngine\bin\RelWithDebInfo\2.rdc"
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(cap_path, "", None)
    _, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    flat = flatten(ctrl.GetRootActions())
    draws = [x for x in flat if int(x.flags) & int(rd.ActionFlags.Drawcall)]

    print("=== Last 15 draws ===")
    for act in draws[-15:]:
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        vp = st.GetViewport(0)
        sc = st.GetScissor(0)
        rts = st.GetOutputTargets()
        rid = int(rts[0].resource) if rts else 0
        rs = st.GetRasterState()
        depth_clip = getattr(rs, "depthClip", getattr(rs, "depthClipMode", "?"))
        print(
            f"E{act.eventId} idx={act.numIndices} "
            f"vp=({vp.x:.0f},{vp.y:.0f},{vp.width:.0f}x{vp.height:.0f}) "
            f"sc=({sc.x},{sc.y},{sc.width}x{sc.height}) RT={res_name(ctrl, rid)} "
            f"depthClip={depth_clip}"
        )

    print("\n=== Axis E831 ===")
    ctrl.SetFrameEvent(831, False)
    print("NDC:", mesh_ndc(ctrl))
    st = ctrl.GetPipelineState()
    rs = st.GetRasterState()
    depth_clip = getattr(rs, "depthClip", getattr(rs, "depthClipMode", "?"))
    print(f"depthClip={depth_clip} cull={rs.cullMode}")

    print("\n=== Draws with scene scissor (410,131) ===")
    scene_sc = []
    for act in draws:
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        sc = st.GetScissor(0)
        if sc.x == 410 and sc.y == 131:
            vp = st.GetViewport(0)
            rid = int(st.GetOutputTargets()[0].resource) if st.GetOutputTargets() else 0
            scene_sc.append((act.eventId, act.numIndices, vp.width, vp.height, res_name(ctrl, rid)))
    print(f"count={len(scene_sc)}")
    for row in scene_sc[:25]:
        print(f"  E{row[0]} idx={row[1]} vp={row[2]:.0f}x{row[3]:.0f} RT={row[4]}")

    print("\n=== Fullscreen triangle (idx=3) to swapchain ===")
    for act in draws:
        if act.numIndices != 3:
            continue
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        rts = st.GetOutputTargets()
        if not rts:
            continue
        rid = int(rts[0].resource)
        if "Swapchain" in res_name(ctrl, rid):
            vp = st.GetViewport(0)
            print(f"  E{act.eventId} vp=({vp.x:.0f},{vp.y:.0f},{vp.width:.0f}x{vp.height:.0f})")

    print("\n=== RP1 mesh draws with scene scissor in G-buffer ===")
    for act in draws:
        if act.numIndices < 100:
            continue
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        sc = st.GetScissor(0)
        if sc.x != 410 or sc.y != 131:
            continue
        rid = int(st.GetOutputTargets()[0].resource) if st.GetOutputTargets() else 0
        if "333" in res_name(ctrl, rid) or "334" in res_name(ctrl, rid) or "335" in res_name(ctrl, rid):
            vp = st.GetViewport(0)
            print(f"  E{act.eventId} idx={act.numIndices} vp=({vp.x:.0f},{vp.y:.0f},{vp.width:.0f}x{vp.height:.0f}) RT={res_name(ctrl, rid)}")

    print("\n=== Pixel history scene center (1421,790) after axis ===")
    px, py = 1421, 790
    swap_rid = None
    for r in ctrl.GetResources():
        if "Swapchain" in r.name:
            swap_rid = int(r.resourceId)
            break
    ctrl.SetFrameEvent(831, True)
    try:
        hist = ctrl.PixelHistory(swap_rid, px, py, rd.Subresource(), rd.CompType.Typeless)
        print(f"events={len(hist)}")
        for h in hist[-10:]:
            c = h.shaderOut.col.value
            print(
                f"  E{h.eventId} passed={h.passed} out=({c[0]:.3f},{c[1]:.3f},{c[2]:.3f},{c[3]:.3f})"
            )
    except Exception as exc:
        print(f"failed: {exc}")

    cap.Shutdown()
    rd.ShutdownReplay()


if __name__ == "__main__":
    main()
