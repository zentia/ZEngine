"""把单个 glb 转成 FBX（Blender headless 脚本，供 restore_scene.py 调用）。

用法（由 restore_scene.py 的 glb_to_fbx() 包装，勿手跑）：
    blender --background --python glb_to_fbx.py -- <glb> <fbx>

坐标系约定：glb 是 glTF Y-up；Blender importer 已转成 Z-up 局部坐标。
本脚本不做任何 anchor/旋转/缩放摆放 —— 只把局部网格原样导出成 FBX，
摆放交给 Unity 场景的 Transform（与现网 FBX 原始资产同一口径）。

材质：FBX 内不烘焙贴图字节（贴图由 restore_scene.py 单独抽成 PNG，
Unity 侧材质走现网 zboard_*.mat）；只保留网格几何 + UV + 法线。
"""

import sys

import bpy


def main() -> None:
    argv = sys.argv
    if "--" not in argv:
        raise SystemExit("usage: blender --background --python glb_to_fbx.py -- <glb> <fbx>")
    args = argv[argv.index("--") + 1:]
    if len(args) != 2:
        raise SystemExit("expected <glb> <fbx>, got %d args" % len(args))
    glb, out = args[0], args[1]

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb)

    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("no mesh in %s" % glb)

    # 导出所有 mesh（含层级）；局部坐标、不摆 anchor。
    bpy.ops.export_scene.fbx(
        filepath=out,
        use_selection=False,
        object_types={"MESH"},
        use_mesh_modifiers=True,
        mesh_smooth_type="OFF",     # 保留法线，不重算平滑
        use_tspace=False,           # 不烘焙切线（Unity 导入时算）
        path_mode="AUTO",
        axis_forward="-Z",          # Blender -Z forward -> glTF/Unity 常见约定
        axis_up="Y",                # FBX 用 Y-up（Unity 原生约定）
    )
    print("GLB_TO_FBX_OK:", out)


if __name__ == "__main__":
    main()
