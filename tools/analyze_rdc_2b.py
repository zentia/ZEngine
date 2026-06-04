import os
import sys
from collections import Counter

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


def main():
    cap_path = sys.argv[1] if len(sys.argv) > 1 else r"E:\Engine\ZEngine\bin\RelWithDebInfo\2.rdc"
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(cap_path, "", None)
    _, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    flat = flatten(ctrl.GetRootActions())
    draws = [x for x in flat if int(x.flags) & int(rd.ActionFlags.Drawcall)]

    print("=== Scissor histogram (all draws) ===")
    sc_hist = Counter()
    for act in draws:
        ctrl.SetFrameEvent(act.eventId, False)
        sc = ctrl.GetPipelineState().GetScissor(0)
        sc_hist[(sc.x, sc.y, sc.width, sc.height)] += 1
    for key, n in sc_hist.most_common(20):
        print(f"  sc={key}: {n}")

    print("\n=== G-buffer RT333/334/335 draws by scissor ===")
    gb_sc = Counter()
    gb_samples = {}
    for act in draws:
        ctrl.SetFrameEvent(act.eventId, False)
        st = ctrl.GetPipelineState()
        rts = st.GetOutputTargets()
        if not rts:
            continue
        name = res_name(ctrl, int(rts[0].resource))
        if "333" not in name and "334" not in name and "335" not in name:
            continue
        sc = st.GetScissor(0)
        key = (sc.x, sc.y, sc.width, sc.height)
        gb_sc[key] += 1
        gb_samples.setdefault(key, []).append(act.eventId)
    for key, n in gb_sc.most_common():
        print(f"  sc={key}: {n} draws e.g. E{gb_samples[key][:5]}")

    print("\n=== Events with markers (first 40) ===")
    for act in flat[:60]:
        flags = int(act.flags)
        if flags & int(rd.ActionFlags.PushMarker):
            print(f"  E{act.eventId} PUSH {act.customName}")
        elif flags & int(rd.ActionFlags.Drawcall):
            ctrl.SetFrameEvent(act.eventId, False)
            sc = ctrl.GetPipelineState().GetScissor(0)
            rid = int(ctrl.GetPipelineState().GetOutputTargets()[0].resource)
            print(f"  E{act.eventId} draw idx={act.numIndices} sc={sc.x},{sc.y},{sc.width}x{sc.height} RT={res_name(ctrl, rid)}")

    print("\n=== E104 skybox draw ===")
    ctrl.SetFrameEvent(104, False)
    st = ctrl.GetPipelineState()
    vp = st.GetViewport(0)
    sc = st.GetScissor(0)
    print(f"vp=({vp.x},{vp.y},{vp.width}x{vp.height}) sc=({sc.x},{sc.y},{sc.width}x{sc.height})")
    print(f"RT={res_name(ctrl, int(st.GetOutputTargets()[0].resource))}")

    print("\n=== Combine chain E60-E110 ===")
    for act in draws:
        if 55 <= act.eventId <= 115:
            ctrl.SetFrameEvent(act.eventId, False)
            st = ctrl.GetPipelineState()
            sc = st.GetScissor(0)
            rid = int(st.GetOutputTargets()[0].resource) if st.GetOutputTargets() else 0
            print(f"E{act.eventId} idx={act.numIndices} sc=({sc.x},{sc.y},{sc.width}x{sc.height}) RT={res_name(ctrl, rid)}")

    cap.Shutdown()
    rd.ShutdownReplay()


if __name__ == "__main__":
    main()
