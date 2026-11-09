// ZEngine 参数化棋盘 -> Unity 高配（HD）场景生成器（trunk / Unity 2022.3.5f1）
//
// 部署：把本文件放到 G:\OSGame\Client_proj\Program\GameScripts\Assets\Editor\ZEngine\
//       把 build/board.zbrd / board.manifest.json / board-spec.json / style-pack.json
//       放到 G:\OSGame\Client_proj\Project\Assets\ZEngineChessboard\
// 菜单：`ZEngine/Chessboard Diagnostics (read-only)` 与
// `ZEngine/Verify Chessboard ZBRD`（都只读）。生成已改为离线：
// `unity_scene.py` 写 .unity，`deploy_to_trunk.py` 部署，见 unity_scene.py 文件头。
//
// 交付物是 Assets/Scenes/pvp/ART_OS_<id>_HD.unity 这一个**场景**，不是 prefab：
// * 现网 pvp 目录下配的是 .unity 场景族 ART_OS_ACxx_{HD,High,Mid,Low}.unity；
//   SceneTools.cs:26 的 ART_NODE="Artist" 说明场景序列化的输入是场景里名为
//   Artist 的根节点，序列化后才产出运行时加载的 SceneExport/Artist/*_Artist.asset。
// * High/Mid/Low 由项目既有自动降配流程（ArtTools/4.场景工具/--->HD转高中低配，
//   或 FastAutoOptimization）从 HD 生成后由美术手调，本线不产出、也不碰配置表。
// * 场景骨架：Artist（序列化输入根）/ SceneMesh（挂 Project8ArtField，fieldRoot
//   指向自身）/ Light_Group + DirectionalLight / RenderSettingGroup（留给美术填
//   RenderSettings / Volume / 天空盒）。棋盘几何直接建在 SceneMesh 下，
//   **不创建任何 prefab 资产**：OSGame 定制引擎的 prefab 异步导入会在 worker
//   线程触发 "Fatal Error! Graphics device is null." 并让 Editor exit(0)。
// * 只消费 ZBRD 的 HD 档部件（lod_mask & 1）。
// * 几何公式只在 Python 侧实现一次，本脚本只读 ZBRD，不重新算棋盘格。
// * 核心区是一整块连续台面（7x8 只是逻辑落点，不是 56 个可见方块）；
//   外围装饰由 style-pack.json 决定，ZBRD 里以 decoration_/interactable_ 前缀区分。
// * 不硬引用 OSGame 的任何类型：Project8ArtField 通过反射按名字查找。
// * three.js 是 Y-up 右手系，Unity 是 Y-up 左手系，读字节时统一做
//   x -> -x / normal.x -> -normal.x / uv.y -> 1-uv.y / 反转三角形绕序。

#if UNITY_EDITOR
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace ZEngine.Chessboard.Editor
{
    [Serializable]
    public class BoardGridJson
    {
        public string layout;
        public int columns;
        public int rows;
        public float cell_width_m;
        public float cell_height_m;
        public string shape;
        public string visual_kind;
        public bool logical_grid_is_visual;
        public string cell_marks;
    }

    // 冻结核心契约：7x8 只是逻辑落点，核心区是一整块连续台面。
    [Serializable]
    public class BoardCoreContractJson
    {
        public string visual_kind;
        public bool logical_grid_is_visual;
    }

    // 可变风格包：只描述外围装饰，不得改动核心几何。
    [Serializable]
    public class BoardStylePackJson
    {
        public string name;
        public string generated_by;
        public string status;
        public int decorations;
        public int interactables;
    }

    // 交付场景契约：只产一个高配 HD .unity 场景。
    [Serializable]
    public class BoardSceneJson
    {
        public string path;
        public string quality;
        public string artist_root;
        public string field_node;
        public string template;
        public string note;
    }

    [Serializable]
    public class BoardLightingJson
    {
        public float[] directional_color;
        public float directional_intensity;
        public float[] directional_euler;
        public float[] ambient_color;
        public float ambient_intensity;
    }

    [Serializable]
    public class BoardSkyJson
    {
        public float[] top_color;
        public float[] horizon_color;
        public float[] bottom_color;
        public float radius_m;
    }

    [Serializable]
    public class BoardUnityJson
    {
        public string material_template;
        public string[] color_properties;
        public string note;
    }

    [Serializable]
    public class BoardLodJson
    {
        public int parts;
        public int vertices;
        public float star_scale;
    }

    [Serializable]
    public class BoardLodsJson
    {
        public BoardLodJson HD;
        public BoardLodJson High;
        public BoardLodJson Mid;
        public BoardLodJson Low;
    }

    [Serializable]
    public class BoardManifestJson
    {
        public string name;
        public string skin_hint;
        public string root_prefix;
        public string sha256;
        public int parts;
        public string delivered_lod;
        public int scene_parts;
        public BoardGridJson grid;
        public BoardSceneJson scene;
        public BoardCoreContractJson core_contract;
        public BoardStylePackJson style_pack;
        public BoardLightingJson lighting;
        public BoardSkyJson sky;
        public BoardUnityJson unity;
        public BoardLodsJson lods;
        public string[] core_parts;
        public string[] variable_parts;
    }

    [Serializable]
    public class ReportEntry
    {
        public string scene;
        public string quality;
        public string artist_root;
        public string field_node;
        public string content_node;
        public bool art_field_attached;
        public int parts;
        public int core_parts;
        public int variable_parts;
        public int vertices;
        public string[] interactable_tags;
    }

    [Serializable]
    public class GenerateReport
    {
        public string schema = "zengine.chessboard.unity-report/v2";
        public string spec_name;
        public string delivery = "one HD .unity scene; lower tiers via OSGame auto-downgrade; config tables untouched";
        public string zbrd_sha256;
        public string coordinate_conversion = "threejs_yup_rhs_m -> unity_yup_lhs_m";
        public string core_visual_kind;
        public string style_pack;
        public string style_pack_status;
        public string material_template;
        public string material_shader;
        public List<ReportEntry> entries = new List<ReportEntry>();
    }

    internal sealed class PartData
    {
        public string Name;
        public int Kind;
        public int LodMask;
        public string Material;
        public string Tag;
        public Color Color;
        public Vector3[] Positions;
        public Vector3[] Normals;
        public Vector2[] Uvs;
        public int[] Indices;
    }

    public static class ZChessboardGenerator
    {
        private const string Root = "Assets/ZEngineChessboard";
        private const string ZbrdPath = Root + "/board.zbrd";
        private const string ManifestPath = Root + "/board.manifest.json";
        private const string MeshDir = Root + "/Meshes";
        private const string MatDir = Root + "/Materials";
        private const string DefaultSceneDir = "Assets/Scenes/pvp";
        private const string DefaultArtistNode = "Artist";
        private const string DefaultFieldNode = "SceneMesh";
        private const uint ZbrdMagic = 0x4452425A;
        private const int HdBit = 1;

        private const int KindGrid = 0;
        private const int KindFrame = 1;
        private const int KindSky = 2;
        private const int KindDecoration = 3;
        private const int KindInteractable = 4;

        private static readonly string[] LodNames = { "HD", "High", "Mid", "Low" };
        private static readonly int[] LodBits = { 1, 2, 4, 8 };

        [MenuItem("ZEngine/Verify Chessboard ZBRD")]
        public static void Verify()
        {
            try
            {
                BoardManifestJson manifest = ReadManifest();
                List<PartData> parts = ReadZbrd(ZbrdPath);
                string digest = Sha256(ZbrdPath);
                if (parts.Count != manifest.parts)
                    throw new Exception("part count mismatch: ZBRD=" + parts.Count
                                        + " manifest=" + manifest.parts);
                if (digest != manifest.sha256)
                    throw new Exception("sha256 mismatch: " + digest + " != " + manifest.sha256);
                VerifyCoreContract(manifest, parts);
                for (int i = 0; i < LodNames.Length; i++)
                {
                    int count = parts.Count(p => (p.LodMask & LodBits[i]) != 0);
                    BoardLodJson expected = LodOf(manifest, i);
                    if (expected != null && count != expected.parts)
                        throw new Exception(LodNames[i] + " part count mismatch");
                }
                Debug.Log("[ZChessboard] VERIFY OK: " + manifest.name
                          + ", " + parts.Count + " parts (HD scene consumes "
                          + parts.Count(p => (p.LodMask & HdBit) != 0) + "), sha256="
                          + digest.Substring(0, 16));
            }
            catch (Exception exception)
            {
                Debug.LogError("[ZChessboard] VERIFY FAILED: " + exception.Message);
            }
        }

        // 只读诊断菜单。HD 场景改由 unity_scene.py 离线写 .unity、
        // deploy_to_trunk.py 直接部署；Unity 里不再跑任何生成逻辑。
        // 原因：OSGame 定制引擎 GfxDevice.cpp:189-195 在 GFX_TLS_DEVICE 下，
        // 非主线程 GetGfxDevice() -> FatalErrorMsg("Graphics device is null.")
        // -> exit(0)，菜单回调里的 AssetDB 导入 / NewScene 会触发，且无 dump。
        [MenuItem("ZEngine/Chessboard Diagnostics (read-only)")]
        public static void Generate()
        {
            try
            {
                BoardManifestJson manifest = ReadManifest();
                List<PartData> parts = ReadZbrd(ZbrdPath);
                if (parts.Count != manifest.parts)
                    throw new Exception("part count mismatch with manifest");
                if (Sha256(ZbrdPath) != manifest.sha256)
                    throw new Exception("ZBRD sha256 mismatch; re-run zboard.py --variants");
                VerifyCoreContract(manifest, parts);
                int hdCount = parts.Count(p => (p.LodMask & HdBit) != 0);
                string path = manifest.scene == null ? "" : manifest.scene.path;
                bool onDisk = !string.IsNullOrEmpty(path) && File.Exists(path);
                Debug.Log("[ZChessboard] READ-ONLY diagnostics: " + parts.Count
                    + " parts / " + hdCount + " HD; scene " + path
                    + (onDisk ? " EXISTS on disk" : " MISSING on disk"));
                Debug.Log("[ZChessboard] This menu writes NOTHING. The HD .unity scene "
                    + "is generated offline by unity_scene.py and deployed by "
                    + "deploy_to_trunk.py, which avoids the GFX_TLS_DEVICE fatal.");
            }
            catch (Exception exception)
            {
                Debug.LogError("[ZChessboard] DIAGNOSTICS FAILED: " + exception.Message);
            }
        }

        // 保留的 Editor 内生成逻辑，不暴露菜单：只适用于不受 GFX_TLS_DEVICE
        // 限制的 Unity。trunk 上请勿点击执行，改用离线交付。
        public static void GenerateInEditorUnsafe()
        {
            try
            {
                BoardManifestJson manifest = ReadManifest();
                List<PartData> parts = ReadZbrd(ZbrdPath);
                if (parts.Count != manifest.parts)
                    throw new Exception("part count mismatch with manifest");
                if (Sha256(ZbrdPath) != manifest.sha256)
                    throw new Exception("ZBRD sha256 mismatch; re-run zboard.py --variants");
                VerifyCoreContract(manifest, parts);
                if (manifest.scene == null || string.IsNullOrEmpty(manifest.scene.path))
                    throw new Exception("manifest.scene.path is required (one HD .unity scene)");
                if (manifest.scene.quality != "HD")
                    throw new Exception("only the HD scene is delivered here; High/Mid/Low "
                        + "come from OSGame's auto-downgrade tools");
                string scenePath = manifest.scene.path.Replace('\\', '/');
                string artistName = string.IsNullOrEmpty(manifest.scene.artist_root)
                    ? DefaultArtistNode : manifest.scene.artist_root;
                string fieldName = string.IsNullOrEmpty(manifest.scene.field_node)
                    ? DefaultFieldNode : manifest.scene.field_node;

                EnsureFolder(Root);
                EnsureFolder(MeshDir);
                EnsureFolder(MatDir);
                EnsureFolder(Path.GetDirectoryName(scenePath).Replace('\\', '/'));

                // 只消费 HD 档部件：交付物是高配场景，其余档位由项目工具派生。
                List<PartData> selected = parts.Where(p => (p.LodMask & HdBit) != 0).ToList();
                if (selected.Count == 0) throw new Exception("no HD parts in ZBRD");
                if (manifest.scene_parts > 0 && selected.Count != manifest.scene_parts)
                    throw new Exception("HD part count mismatch with manifest.scene_parts");
                ReportStaleAssets(selected);
                LogStep(1, "manifest/ZBRD verified, " + selected.Count + " HD parts");

                var meshCache = new Dictionary<string, Mesh>();
                var materialCache = new Dictionary<string, Material>();
                Material template = LoadMaterialTemplate(manifest);
                var report = new GenerateReport
                {
                    spec_name = manifest.name,
                    zbrd_sha256 = manifest.sha256,
                    core_visual_kind = manifest.core_contract == null
                        ? "" : manifest.core_contract.visual_kind,
                    style_pack = manifest.style_pack == null ? "" : manifest.style_pack.name,
                    style_pack_status = manifest.style_pack == null ? "" : manifest.style_pack.status,
                    material_template = template == null ? "" : AssetDatabase.GetAssetPath(template),
                    material_shader = template == null || template.shader == null
                        ? "FALLBACK (may render black)" : template.shader.name,
                };

                // 崩溃修复：绝不在这个菜单回调里 SaveAsPrefabAsset。
                // OSGame 定制引擎开了 "Prefab file async load"，prefab 导入会在
                // worker 线程初始化渲染资源，触发
                //   "Fatal Error! Graphics device is null." -> Unity exit(0)
                // 这不是 native crash，所以不写 dump，Editor 表现为"闪退"。
                // 同时把所有 mesh/material 写入合并为一次 AssetDB refresh，避免
                // 连续 DeleteAsset/CreateAsset 触发十几次 RefreshInternalV2 以及
                // "sortedQueueGuids entry size does not match" 告警。
                LogStep(2, "creating/updating mesh + material assets in ONE batch");
                AssetDatabase.StartAssetEditing();
                try
                {
                    foreach (PartData part in selected)
                    {
                        GetMesh(part, meshCache);
                        GetMaterial(part, materialCache, manifest, template);
                    }
                }
                finally
                {
                    AssetDatabase.StopAssetEditing();
                }
                AssetDatabase.SaveAssets();
                LogStep(3, "assets ready; building the HD scene");

                // 棋盘几何直接建成场景里的 GameObject，不再经过 prefab 资产。
                if (EditorSceneManager.GetActiveScene().isDirty)
                    Debug.LogWarning("[ZChessboard] active scene has unsaved changes; "
                        + "NewScene(Single) may prompt to save. Save your scene first if "
                        + "the editor appears to hang at step 3.");
                Scene scene = OpenOrCreateScene(scenePath, manifest.scene.template);
                GameObject artist = ReplaceArtistRoot(scene, artistName);
                var field = new GameObject(fieldName);
                field.transform.SetParent(artist.transform, false);
                var lightGroup = new GameObject("Light_Group");
                lightGroup.transform.SetParent(artist.transform, false);
                var lightGo = new GameObject("DirectionalLight");
                lightGo.transform.SetParent(lightGroup.transform, false);
                Light light = lightGo.AddComponent<Light>();
                light.type = LightType.Directional;
                light.intensity = manifest.lighting != null
                    ? manifest.lighting.directional_intensity : 1.0f;
                light.color = ColorOf(manifest.lighting != null
                    ? manifest.lighting.directional_color : null, Color.white);
                float[] euler = manifest.lighting != null
                    ? manifest.lighting.directional_euler : null;
                lightGo.transform.eulerAngles = euler != null && euler.Length >= 3
                    ? new Vector3(euler[0], euler[1], euler[2])
                    : new Vector3(48f, -32f, 0f);
                var renderGroup = new GameObject("RenderSettingGroup");
                renderGroup.transform.SetParent(artist.transform, false);

                string contentNode = BuildBoardHierarchy(manifest, selected, field.transform,
                    meshCache, materialCache);

                // Project8ArtField 挂在 SceneMesh 上，fieldRoot 指向自身（现网 HD 场景同构）。
                bool attached = AttachArtField(field, field, lightGo);

                EditorSceneManager.MarkSceneDirty(scene);
                if (!EditorSceneManager.SaveScene(scene, scenePath))
                    throw new Exception("SaveScene failed: " + scenePath);
                LogStep(4, "HD scene saved: " + scenePath);

                int coreCount = selected.Count(p => IsCorePart(p.Name));
                int variableCount = selected.Count(p => IsVariablePart(p.Name));
                report.entries.Add(new ReportEntry
                {
                    scene = scenePath,
                    quality = "HD",
                    artist_root = artistName,
                    field_node = fieldName,
                    content_node = contentNode,
                    art_field_attached = attached,
                    parts = selected.Count,
                    core_parts = coreCount,
                    variable_parts = variableCount,
                    vertices = selected.Sum(p => p.Positions.Length),
                    interactable_tags = selected.Where(p => p.Kind == KindInteractable)
                        .Select(p => p.Tag).ToArray(),
                });

                string reportPath = Path.Combine(Application.dataPath,
                    "ZEngineChessboard/ZChessboardGenerateReport.json");
                File.WriteAllText(reportPath, JsonUtility.ToJson(report, true), Encoding.UTF8);
                AssetDatabase.SaveAssets();
                LogStep(5, "done");

                Debug.Log("[ZChessboard] HD scene: " + selected.Count + " parts ("
                    + coreCount + " frozen core + " + variableCount + " style-pack) -> " + scenePath);
                Debug.Log("[ZChessboard] DONE. Hand-off notes (NOT automated):\n"
                    + "1) 场景里 Artist 是 SceneTools 的序列化输入根；SceneMesh 已挂\n"
                    + "   Project8ArtField（fieldRoot/directionalLight 为 GameObject，\n"
                    + "   ChangeStencil=true，Stencil ref/mask=4 / Comp NotEqual）。\n"
                    + "2) RenderSettingGroup 是空节点：RenderSettings / Volume / 天空盒 /\n"
                    + "   后处理需美术手动补，或把 spec.scene.template 指向现成 HD 场景继承。\n"
                    + "3) High/Mid/Low 由项目自动降配流程从本 HD 场景生成后手调，本线不产出；\n"
                    + "   配置表（ResSceneCfg / ResPersonaliseChessboardDataBin）不动。\n"
                    + "4) 需要运行时资产时再跑 SGameTools/场景序列化/输出美术节点，\n"
                    + "   产出 SceneExport/Artist/*_Artist.asset + *_core.bytessv。");
            }
            catch (Exception exception)
            {
                Debug.LogError("[ZChessboard] GENERATE FAILED: " + exception);
            }
        }

        // 逐步日志：万一 Editor 再次主动退出，Editor.log 里能看出停在哪一步。
        private static void LogStep(int index, string message)
        {
            Debug.Log("[ZChessboard] step " + index + "/5: " + message);
        }

        // 棋盘几何直接建进场景的 SceneMesh 下，不创建任何 prefab 资产。
        // mesh/material 已在批量 AssetDB 编辑阶段写好，这里只做场景装配。
        private static string BuildBoardHierarchy(BoardManifestJson manifest,
            List<PartData> selected, Transform field,
            Dictionary<string, Mesh> meshCache, Dictionary<string, Material> materialCache)
        {
            var root = new GameObject(Sanitize(manifest.root_prefix));
            root.transform.SetParent(field, false);
            var boardRoot = new GameObject("BoardMesh");
            boardRoot.transform.SetParent(root.transform, false);
            var skyRoot = new GameObject("Sky");
            skyRoot.transform.SetParent(root.transform, false);
            foreach (PartData part in selected)
            {
                Mesh mesh = meshCache[part.Name];
                Material material = materialCache[part.Material];
                var go = new GameObject(part.Name);
                go.transform.SetParent(part.Kind == KindSky
                    ? skyRoot.transform : boardRoot.transform, false);
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                go.AddComponent<MeshRenderer>().sharedMaterial = material;
                if (part.Kind == KindInteractable)
                    go.name = part.Name + " [" + part.Tag + "]";
            }
            return root.name;
        }

        // template=null -> 新建空场景（骨架干净，环境设置交美术）；
        // template=现成 _HD.unity -> 复制该场景到目标路径以继承 RenderSettings/Volume，
        // 随后 Artist 根会被整体替换成新棋盘（其它根保留，残留引用需人工检查）。
        private static Scene OpenOrCreateScene(string scenePath, string template)
        {
            if (!string.IsNullOrEmpty(template))
            {
                string source = template.Replace('\\', '/');
                if (!File.Exists(source))
                    throw new Exception("scene.template not found: " + source);
                Scene copied = EditorSceneManager.OpenScene(source, OpenSceneMode.Single);
                if (!EditorSceneManager.SaveScene(copied, scenePath, false))
                    throw new Exception("could not copy template scene to " + scenePath);
                Debug.Log("[ZChessboard] copied HD template " + source + " -> " + scenePath);
                return EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
            }
            return EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        }

        private static GameObject ReplaceArtistRoot(Scene scene, string artistName)
        {
            foreach (GameObject go in scene.GetRootGameObjects())
                if (go.name == artistName) UnityEngine.Object.DestroyImmediate(go);
            var artist = new GameObject(artistName);
            if (artist.scene != scene)
                UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(artist, scene);
            return artist;
        }

        private static readonly string[] CorePrefixes =
            { "core_slab", "frame_", "anchor_", "sky@" };
        private static readonly string[] VariablePrefixes =
            { "cell_marks", "decoration_", "interactable_" };

        private static bool IsCorePart(string name)
        {
            foreach (string prefix in CorePrefixes)
                if (name.StartsWith(prefix)) return true;
            return false;
        }

        private static bool IsVariablePart(string name)
        {
            foreach (string prefix in VariablePrefixes)
                if (name.StartsWith(prefix)) return true;
            return false;
        }

        // 现网皮肤核心区是一整块连续台面；早期版本误把 7x8 逻辑网格当视觉几何，
        // 生成了 56 个独立 cell_* 方块。这里守住分层契约，防止旧 ZBRD 混进来。
        private static void VerifyCoreContract(BoardManifestJson manifest, List<PartData> parts)
        {
            if (manifest.core_contract == null
                || manifest.core_contract.visual_kind != "continuous_slab"
                || manifest.core_contract.logical_grid_is_visual)
                throw new Exception("manifest declares no frozen continuous_slab core");
            if (parts.Count(p => p.Name == "core_slab") != 1)
                throw new Exception("expected exactly one continuous core_slab");
            if (parts.Any(p => p.Name.StartsWith("cell_") && p.Name != "cell_marks"))
                throw new Exception("logical grid cells must NOT be visible geometry");
            if (manifest.core_parts != null && manifest.variable_parts != null
                && manifest.core_parts.Length + manifest.variable_parts.Length != parts.Count)
                throw new Exception("core_parts + variable_parts != total parts");
        }

        private static BoardLodJson LodOf(BoardManifestJson manifest, int index)
        {
            if (manifest.lods == null) return null;
            switch (index)
            {
                case 0: return manifest.lods.HD;
                case 1: return manifest.lods.High;
                case 2: return manifest.lods.Mid;
                default: return manifest.lods.Low;
            }
        }

        private static BoardManifestJson ReadManifest()
        {
            if (!File.Exists(ManifestPath))
                throw new Exception("missing " + ManifestPath + " (run deploy_to_trunk.py --apply first)");
            BoardManifestJson manifest = JsonUtility.FromJson<BoardManifestJson>(
                File.ReadAllText(ManifestPath, Encoding.UTF8));
            if (manifest == null || string.IsNullOrEmpty(manifest.root_prefix))
                throw new Exception("bad manifest JSON");
            return manifest;
        }

        private static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(path))
            {
                byte[] hash = sha.ComputeHash(stream);
                var builder = new StringBuilder();
                foreach (byte value in hash) builder.Append(value.ToString("x2"));
                return builder.ToString();
            }
        }

        private static string ReadString(BinaryReader reader)
        {
            uint length = reader.ReadUInt32();
            if (length > (1 << 20)) throw new Exception("oversized ZBRD string");
            return Encoding.UTF8.GetString(reader.ReadBytes((int)length));
        }

        private static List<PartData> ReadZbrd(string path)
        {
            if (!File.Exists(path))
                throw new Exception("missing " + path + " (run deploy_to_trunk.py --apply first)");
            var parts = new List<PartData>();
            using (var stream = File.OpenRead(path))
            using (var reader = new BinaryReader(stream))
            {
                uint magic = reader.ReadUInt32();
                uint version = reader.ReadUInt32();
                uint count = reader.ReadUInt32();
                uint flags = reader.ReadUInt32();
                if (magic != ZbrdMagic) throw new Exception("bad ZBRD magic");
                if (version != 1 || flags != 0) throw new Exception("unsupported ZBRD version/flags");
                if (stream.Length < 16) throw new Exception("ZBRD too short");

                for (uint n = 0; n < count; n++)
                {
                    var part = new PartData { Name = ReadString(reader) };
                    part.Kind = reader.ReadByte();
                    part.LodMask = reader.ReadByte();
                    reader.ReadByte();
                    reader.ReadByte();
                    part.Material = ReadString(reader);
                    part.Tag = ReadString(reader);
                    int vertexCount = checked((int)reader.ReadUInt32());
                    int indexCount = checked((int)reader.ReadUInt32());
                    for (int i = 0; i < 16; i++) reader.ReadSingle(); // matrix（已烘焙，恒为单位阵）
                    part.Color = new Color(reader.ReadSingle(), reader.ReadSingle(),
                                           reader.ReadSingle(), reader.ReadSingle());

                    part.Positions = new Vector3[vertexCount];
                    part.Normals = new Vector3[vertexCount];
                    part.Uvs = new Vector2[vertexCount];
                    part.Indices = new int[indexCount];

                    // three.js Y-up 右手系 -> Unity Y-up 左手系：反 X + UV V 翻转。
                    for (int i = 0; i < vertexCount; i++)
                        part.Positions[i] = new Vector3(-reader.ReadSingle(),
                            reader.ReadSingle(), reader.ReadSingle());
                    for (int i = 0; i < vertexCount; i++)
                        part.Normals[i] = new Vector3(-reader.ReadSingle(),
                            reader.ReadSingle(), reader.ReadSingle()).normalized;
                    for (int i = 0; i < vertexCount; i++)
                        part.Uvs[i] = new Vector2(reader.ReadSingle(),
                            1f - reader.ReadSingle());
                    for (int i = 0; i < indexCount; i++)
                        part.Indices[i] = checked((int)reader.ReadUInt32());
                    // 反手性会让三角形绕序反向，逐三角交换后两个索引。
                    for (int t = 0; t + 2 < indexCount; t += 3)
                    {
                        int swap = part.Indices[t + 1];
                        part.Indices[t + 1] = part.Indices[t + 2];
                        part.Indices[t + 2] = swap;
                    }
                    if (part.Indices.Any(index => index >= vertexCount))
                        throw new Exception("out-of-range index in part " + part.Name);
                    parts.Add(part);
                }
                if (stream.Position != stream.Length)
                    throw new Exception("ZBRD trailing bytes");
            }
            return parts;
        }

        private static Color ColorOf(float[] rgba, Color fallback)
        {
            if (rgba == null || rgba.Length < 3) return fallback;
            return new Color(rgba[0], rgba[1], rgba[2],
                             rgba.Length > 3 ? rgba[3] : 1f);
        }

        private static string Sanitize(string name)
        {
            var builder = new StringBuilder();
            foreach (char c in name)
                builder.Append(char.IsLetterOrDigit(c) || c == '_' || c == '-' ? c : '_');
            return builder.ToString();
        }

        private static void EnsureFolder(string path)
        {
            string[] segments = path.Split('/');
            string current = segments[0];
            for (int i = 1; i < segments.Length; i++)
            {
                string next = current + "/" + segments[i];
                if (!AssetDatabase.IsValidFolder(next))
                    AssetDatabase.CreateFolder(current, segments[i]);
                current = next;
            }
        }

        // 早期 56-cell 版本在 trunk 里留下过 74 个 mesh / 14 个 material。
        // 本脚本只按当前 part 名字覆盖同名资源，不会自动删除孤儿的 cell_*。
        // 这里只报告、不删除：删除用户工程资产必须人工确认。
        private static void ReportStaleAssets(List<PartData> parts)
        {
            var expectedMeshes = new HashSet<string>(parts.Select(p => Sanitize(p.Name)));
            var expectedMaterials = new HashSet<string>(parts.Select(p => Sanitize(p.Material)));
            int staleMeshes = 0;
            int staleMaterials = 0;
            foreach (string guid in AssetDatabase.FindAssets("t:Mesh", new[] { MeshDir }))
            {
                string stem = Path.GetFileNameWithoutExtension(AssetDatabase.GUIDToAssetPath(guid));
                if (!expectedMeshes.Contains(stem)) staleMeshes += 1;
            }
            foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { MatDir }))
            {
                string stem = Path.GetFileNameWithoutExtension(AssetDatabase.GUIDToAssetPath(guid));
                if (!expectedMaterials.Contains(stem)) staleMaterials += 1;
            }
            if (staleMeshes > 0 || staleMaterials > 0)
                Debug.LogWarning("[ZChessboard] stale assets from the old 56-cell build: "
                    + staleMeshes + " meshes / " + staleMaterials + " materials under "
                    + Root + ". Delete Meshes/ and Materials/ manually before running "
                    + "SceneTools serialization so SceneExportChecker does not collect "
                    + "orphaned cell_* resources.");
        }

        // 复用已有资产并原地覆盖，避免 DeleteAsset 触发额外 AssetDB refresh。
        private static Mesh GetMesh(PartData part, Dictionary<string, Mesh> cache)
        {
            Mesh mesh;
            if (cache.TryGetValue(part.Name, out mesh)) return mesh;
            string path = MeshDir + "/" + Sanitize(part.Name) + ".asset";
            mesh = AssetDatabase.LoadAssetAtPath<Mesh>(path);
            bool isNew = mesh == null;
            if (isNew) mesh = new Mesh { name = Sanitize(part.Name) };
            else mesh.Clear();
            mesh.SetVertices(new List<Vector3>(part.Positions));
            mesh.SetNormals(new List<Vector3>(part.Normals));
            mesh.SetUVs(0, new List<Vector2>(part.Uvs));
            mesh.SetTriangles(new List<int>(part.Indices), 0);
            mesh.RecalculateBounds();
            if (isNew) AssetDatabase.CreateAsset(mesh, path);
            else EditorUtility.SetDirty(mesh);
            cache[part.Name] = mesh;
            return mesh;
        }

        private static Material LoadMaterialTemplate(BoardManifestJson manifest)
        {
            string path = manifest.unity != null
                && !string.IsNullOrEmpty(manifest.unity.material_template)
                ? manifest.unity.material_template
                : "Assets/CustomResources/OSG_Default.mat";
            Material template = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (template == null)
                Debug.LogWarning("[ZChessboard] material template missing: " + path
                    + ". Falling back to Shader.Find — built-in Standard renders BLACK "
                    + "under OSGameForwardRenderer. Fix unity.material_template in board-spec.json.");
            else
                Debug.Log("[ZChessboard] material template: " + path
                    + " / shader=" + (template.shader == null ? "null" : template.shader.name));
            return template;
        }

        private static Material GetMaterial(PartData part,
            Dictionary<string, Material> cache, BoardManifestJson manifest, Material template)
        {
            string key = part.Material;
            Material material;
            if (cache.TryGetValue(key, out material)) return material;
            string path = MatDir + "/" + Sanitize(key) + ".mat";
            material = AssetDatabase.LoadAssetAtPath<Material>(path);
            bool isNew = material == null;
            bool sky = part.Kind == KindSky;
            if (isNew)
            {
                if (template != null)
                {
                    // 克隆工程默认材质：继承自定义 SRP 认识的 shader、keywords、
                    // Stencil 与渲染队列。绝不能自己 Shader.Find（URP/Lit 在 trunk
                    // 找不到，回退内置 Standard 后在 OSGameForwardRenderer 下是黑的）。
                    material = new Material(template);
                }
                else
                {
                    Shader shader = Shader.Find("Universal Render Pipeline/Lit")
                        ?? Shader.Find("Standard") ?? Shader.Find("Sprites/Default");
                    if (shader == null) throw new Exception("no usable shader found");
                    material = new Material(shader);
                }
                material.name = Sanitize(key);
                AssetDatabase.CreateAsset(material, path);
            }
            else if (template != null && template.shader != null
                     && material.shader != template.shader)
            {
                // 修正旧版本可能留下的 Standard 回退材质。
                material.shader = template.shader;
            }

            Color color = sky && manifest.sky != null
                ? ColorOf(manifest.sky.horizon_color, part.Color) : part.Color;
            string[] properties = manifest.unity != null
                && manifest.unity.color_properties != null
                && manifest.unity.color_properties.Length > 0
                ? manifest.unity.color_properties
                : new[] { "_AlbedoColor", "_Color", "_BaseColor" };
            bool applied = false;
            foreach (string property in properties)
            {
                if (!material.HasProperty(property)) continue;
                material.SetColor(property, color);
                applied = true;
            }
            if (!applied) material.color = color;
            // 天空球法线朝内；shader 暴露 _Cull 时关掉剔除，避免内外表现不一致。
            if (sky && material.HasProperty("_Cull")) material.SetFloat("_Cull", 0f);
            EditorUtility.SetDirty(material);
            cache[key] = material;
            return material;
        }

        // 现网 HD 场景里 Project8ArtField 挂在 SceneMesh 上、fieldRoot 指向自身，
        // sceneBackGround 为 {fileID: 0}。这里保持同构。
        private static bool AttachArtField(GameObject host, GameObject fieldRoot,
            GameObject directionalLight)
        {
            Type type = null;
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type[] types;
                try { types = assembly.GetTypes(); }
                catch (ReflectionTypeLoadException exception) { types = exception.Types; }
                catch (Exception) { continue; }
                type = types.FirstOrDefault(t => t != null && t.Name == "Project8ArtField");
                if (type != null) break;
            }
            if (type == null)
            {
                Debug.LogWarning("[ZChessboard] Project8ArtField not found in loaded assemblies. "
                    + "Scene still saved; attach the component manually after deploying to trunk.");
                return false;
            }

            Component component = host.AddComponent(type);
            var serialized = new SerializedObject(component);
            // Project8ArtField.cs:38/41/60 的字段类型是 GameObject，不是
            // Transform / Light。早期版本传组件会被 objectReferenceValue 静默
            // 拒绝，场景落成 {fileID: 0}，运行时报 InitScene artField not ready。
            SetObject(serialized, "fieldRoot", fieldRoot);
            SetObject(serialized, "directionalLight", directionalLight);
            SetObject(serialized, "sceneBackGround", null);
            SetBool(serialized, "ChangeStencil", true);
            SetVector(serialized, "sceneSlotOffset", new Vector3(0f, 0.04f, -0.06f));
            serialized.ApplyModifiedPropertiesWithoutUndo();
            serialized.Update();
            bool ok = true;
            foreach (string name in new[] { "fieldRoot", "directionalLight" })
            {
                SerializedProperty property = serialized.FindProperty(name);
                if (property != null && property.objectReferenceValue == null)
                {
                    Debug.LogError("[ZChessboard] " + name
                        + " is still NULL after apply — scene will not load at runtime");
                    ok = false;
                }
            }
            Debug.Log("[ZChessboard] attached " + type.FullName + " via reflection");
            return ok;
        }

        private static void SetObject(SerializedObject serialized, string name, UnityEngine.Object value)
        {
            SerializedProperty property = serialized.FindProperty(name);
            if (property == null) return;
            property.objectReferenceValue = value;
        }

        private static void SetBool(SerializedObject serialized, string name, bool value)
        {
            SerializedProperty property = serialized.FindProperty(name);
            if (property == null) return;
            property.boolValue = value;
        }

        private static void SetVector(SerializedObject serialized, string name, Vector3 value)
        {
            SerializedProperty property = serialized.FindProperty(name);
            if (property == null) return;
            property.vector3Value = value;
        }
    }
}
#endif
