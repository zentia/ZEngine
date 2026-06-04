"""Analyze ZEditor RenderDoc capture - viewport / compositing focus."""
import os
import sys
from collections import Counter

PYD = r"E:\Engine\ZEngine\tools\renderdoc\x64\Development\pymodules"
DLL = r"E:\Engine\ZEngine\tools\renderdoc\x64\Development"
sys.path.insert(0, PYD)
if hasattr(os, "add_dll_directory"):
    os.add_dll_directory(PYD)
    os.add_dll_directory(DLL)

import renderdoc as rd  # noqa: E402


def flatten(actions, out=None):
    if out is None:
        out = []
    for action in actions:
        out.append(action)
        flatten(action.children, out)
    return out


def resource_name(controller, rid):
    for res in controller.GetResources():
        if int(res.resourceId) == int(rid):
            return res.name
    return str(rid)


def texture_by_id(controller, rid):
    for tex in controller.GetTextures():
        if int(tex.resourceId) == int(rid):
            return tex
    return None


def describe_draw(controller, action):
    controller.SetFrameEvent(action.eventId, False)
    state = controller.GetPipelineState()
    vp = state.GetViewport(0)
    sc = state.GetScissor(0)
    rts = []
    for i, target in enumerate(state.GetOutputTargets()[:4]):
        rid = int(target.resource)
        if rid:
            tex = texture_by_id(controller, rid)
            rts.append(
                f"RT{i}={resource_name(controller, rid)} "
                f"({tex.width}x{tex.height})" if tex else f"RT{i}={rid}"
            )
    dsv = int(state.GetDepthTarget())
    dsv_name = resource_name(controller, dsv) if dsv else "none"
    ds = state.GetDepthTestState()
    rs = state.GetRasterState()
    blends = state.GetColorBlends()
    return {
        "event": action.eventId,
        "indices": action.numIndices,
        "instances": action.numInstances,
        "viewport": (vp.x, vp.y, vp.width, vp.height),
        "scissor": (sc.x, sc.y, sc.width, sc.height),
        "rts": rts,
        "dsv": dsv_name,
        "depth_test": ds.depthEnable,
        "depth_write": ds.depthWrites,
        "cull": rs.cullMode,
        "blend": [b.enabled for b in blends[:2]],
    }


def ndc_bbox(controller):
    vsout = controller.GetPostVSData(rd.MeshDataStage.VSOut, 0, 0)
    if not vsout:
        return None
    ndc_min = [1e9, 1e9, 1e9]
    ndc_max = [-1e9, -1e9, -1e9]
    for vert in vsout[: min(1000, len(vsout))]:
        p = vert.position
        if p.w == 0:
            continue
        x, y, z = p.x / p.w, p.y / p.w, p.z / p.w
        ndc_min[0] = min(ndc_min[0], x)
        ndc_min[1] = min(ndc_min[1], y)
        ndc_min[2] = min(ndc_min[2], z)
        ndc_max[0] = max(ndc_max[0], x)
        ndc_max[1] = max(ndc_max[1], y)
        ndc_max[2] = max(ndc_max[2], z)
    return ndc_min, ndc_max


def main():
    cap_path = sys.argv[1] if len(sys.argv) > 1 else r"E:\Engine\ZEngine\bin\RelWithDebInfo\1.rdc"
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(cap_path, "", None)
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)

    print("=== Textures ===")
    for tex in controller.GetTextures():
        rid = int(tex.resourceId)
        print(f"  {resource_name(controller, rid)}: {tex.width}x{tex.height} {tex.format.Name()}")

    flat = flatten(controller.GetRootActions())
    draws = [a for a in flat if int(a.flags) & int(rd.ActionFlags.Drawcall)]
    clears = [a for a in flat if int(a.flags) & int(rd.ActionFlags.Clear)]
    presents = [a for a in flat if int(a.flags) & int(rd.ActionFlags.Present)]

    print(f"\nEvents: {len(flat)} total, {len(draws)} draws, {len(clears)} clears, {len(presents)} presents")

    hist = Counter(d.numIndices for d in draws)
    print("\n=== Index count histogram ===")
    for count, n in hist.most_common(15):
        print(f"  idx={count}: {n} draws")

    # Group draws by viewport size
    vp_groups = Counter()
    vp_samples = {}
    for action in draws:
        info = describe_draw(controller, action)
        key = (int(info["viewport"][2]), int(info["viewport"][3]))
        vp_groups[key] += 1
        vp_samples.setdefault(key, []).append(info)

    print("\n=== Draws grouped by viewport size ===")
    for (w, h), n in vp_groups.most_common():
        print(f"  {w}x{h}: {n} draws")
        sample = vp_samples[(w, h)][0]
        print(f"    example E{sample['event']} idx={sample['indices']} RT={sample['rts'][:1]}")

    # Swapchain-targeting draws
    print("\n=== Draws targeting Swapchain Image ===")
    swap_draws = []
    for action in draws:
        info = describe_draw(controller, action)
        if any("Swapchain" in rt for rt in info["rts"]):
            swap_draws.append(info)
    print(f"  count={len(swap_draws)}")
    for info in swap_draws[-10:]:
        print(
            f"  E{info['event']} idx={info['indices']} "
            f"vp={info['viewport']} sc={info['scissor']} blend={info['blend']}"
        )

    # Scene-sized viewport draws (not full swapchain UI batch)
    print("\n=== Non-fullscreen viewport draws (likely Scene panel) ===")
    for action in draws:
        info = describe_draw(controller, action)
        w, h = info["viewport"][2], info["viewport"][3]
        if w < 1500 or h < 800:
            if info["indices"] >= 100:
                print(
                    f"  E{info['event']} idx={info['indices']} "
                    f"vp={info['viewport']} RT={info['rts']}"
                )

    # Last draws in frame (compositing order)
    print("\n=== Last 8 draw calls (compositing order) ===")
    for action in draws[-8:]:
        info = describe_draw(controller, action)
        bbox = ndc_bbox(controller)
        bbox_s = ""
        if bbox:
            lo, hi = bbox
            bbox_s = f" ndc_y=[{lo[1]:.2f},{hi[1]:.2f}] ndc_x=[{lo[0]:.2f},{hi[0]:.2f}]"
        print(
            f"  E{info['event']} idx={info['indices']} vp={info['viewport']} "
            f"RT={';'.join(info['rts'][:1])}{bbox_s}"
        )

    # Clears detail
    print("\n=== Clears ===")
    for action in clears:
        controller.SetFrameEvent(action.eventId, False)
        state = controller.GetPipelineState()
        targets = []
        for i, target in enumerate(state.GetOutputTargets()[:4]):
            rid = int(target.resource)
            if rid:
                targets.append(resource_name(controller, rid))
        dsv = int(state.GetDepthTarget())
        print(
            f"  E{action.eventId}: RT={targets} DSV={resource_name(controller, dsv) if dsv else 'none'}"
        )

    cap.Shutdown()
    rd.ShutdownReplay()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
