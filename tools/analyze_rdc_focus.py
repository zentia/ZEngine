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


def mesh_ndc_stats(ctrl):
    fmt = ctrl.GetPostVSData(rd.MeshDataStage.VSOut, 0, 0)
    if not fmt:
        return None
    try:
        mesh = ctrl.GetMeshData(fmt, 0, 0)
    except Exception:
        return None
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
        "p0": (mesh.position[0].x, mesh.position[0].y, mesh.position[0].z),
    }


rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
cap.OpenFile(r"E:\Engine\ZEngine\bin\RelWithDebInfo\1.rdc", "", None)
_, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)

flat = []

def walk(a):
    for x in a:
        flat.append(x)
        walk(x.children)

walk(ctrl.GetRootActions())
draws = [x for x in flat if int(x.flags) & int(rd.ActionFlags.Drawcall)]

print("=== Scene panel geometry (from E830) ===")
ctrl.SetFrameEvent(830, False)
st = ctrl.GetPipelineState()
vp = st.GetViewport(0)
sc = st.GetScissor(0)
print(f"E830 idx=432 vp=({vp.x:.1f},{vp.y:.1f},{vp.width:.1f}x{vp.height:.1f})")
print(f"     sc=({sc.x},{sc.y},{sc.width}x{sc.height})")
rid = int(st.GetOutputTargets()[0].resource)
print(f"     RT={res_name(ctrl, rid)}")
ndc = mesh_ndc_stats(ctrl)
if ndc:
    print(f"     VSOut verts={ndc['count']} ndc_x={ndc['x']} ndc_y={ndc['y']} ndc_z={ndc['z']}")
    print(f"     pos0={ndc['p0']}")

print("\n=== E817 (draw before axis) ===")
ctrl.SetFrameEvent(817, False)
st = ctrl.GetPipelineState()
vp = st.GetViewport(0)
sc = st.GetScissor(0)
print(f"E817 idx=234 vp=({vp.x:.1f},{vp.y:.1f},{vp.width:.1f}x{vp.height:.1f}) sc=({sc.x},{sc.y},{sc.width}x{sc.height})")
ndc = mesh_ndc_stats(ctrl)
if ndc:
    print(f"     VSOut verts={ndc['count']} ndc_y={ndc['y']}")

print("\n=== First deferred draws (E38/E50/E80) ===")
for eid in [38, 50, 80, 120, 200]:
    act = next((x for x in draws if x.eventId == eid), None)
    if not act:
        continue
    ctrl.SetFrameEvent(eid, False)
    st = ctrl.GetPipelineState()
    vp = st.GetViewport(0)
    sc = st.GetScissor(0)
    rid = int(st.GetOutputTargets()[0].resource) if st.GetOutputTargets() else 0
    print(
        f"E{eid} idx={act.numIndices} vp=({vp.x:.0f},{vp.y:.0f},{vp.width:.0f}x{vp.height:.0f}) "
        f"sc=({sc.x},{sc.y},{sc.width}x{sc.height}) RT={res_name(ctrl, rid)}"
    )

print("\n=== Draws with unusual index counts ===")
for act in draws:
    if act.numIndices >= 200:
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        vp = st.GetViewport(0)
        rid = int(st.GetOutputTargets()[0].resource) if st.GetOutputTargets() else 0
        print(
            f"E{act.eventId} idx={act.numIndices} vp=({vp.x:.0f},{vp.y:.0f},{vp.width:.0f}x{vp.height:.0f}) "
            f"RT={res_name(ctrl, rid)}"
        )

print("\n=== Pixel history probe at scene center (E830) ===")
# scene center pixel
px = int(sc.x + sc.width / 2) if False else 921
py = int(131 + 1319 / 2)
ctrl.SetFrameEvent(830, True)
try:
    hist = ctrl.PixelHistory(rid, px, py, rd.Subresource(), rd.CompType.Typeless)
    print(f"  pixel ({px},{py}) history events: {len(hist)}")
    for h in hist[-5:]:
        print(f"    E{h.eventId} passed={h.passed} depth={h.depth} premod=({h.preMod.col.value[0]:.3f},...)")
except Exception as ex:
    print(f"  pixel history failed: {ex}")

cap.Shutdown()
rd.ShutdownReplay()
