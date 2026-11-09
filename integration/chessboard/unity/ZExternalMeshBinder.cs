// ZEngine 棋盘 · 外部单体装饰消费端（Unity）
//
// 数据真源在 ZEngine 侧的 external_mesh.manifest.json：
// 每个实例的 asset_id / anchor / rotation_y_deg / scale / pivot_offset 都由
// external_mesh.py 算好，Unity 只做「读 manifest → 校验 sha256 → 建 Mesh →
// 按锚点摆放 → 关掉代理」。锚点换算与 Blender / Web 完全一致：
//
//     world = anchor + rotY(rotation_y_deg) * scale * (local - pivot_offset)
//
// Unity 2022 没有 glTF 运行库，所以网格从同一份资产的 **ZBRD v1 单部件缓存**
// 读取（由 glb 离线转换，cache_sha256 同样写进契约）。glb 仍会在运行时被
// 完整读取并做 sha256 校验，缓存只是渲染用的派生格式，不是第二份真源。
//
// 失败策略：任何一步失败都保留 ZBRD 代理几何并打 warning，绝不空场景。

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using UnityEngine;

namespace ZEngine.Chessboard
{
    [Serializable]
    public sealed class ZExternalAssetEntry
    {
        public string asset_id;
        public string status;
        public string mesh;
        public string mesh_sha256;
        public string cache;
        public string cache_sha256;
        public string texture;
        public string texture_sha256;
        public float[] pivot_offset;
    }

    [Serializable]
    public sealed class ZExternalPlacementEntry
    {
        public string asset_id;
        public string instance_id;
        public string proxy_part;
        public float[] anchor;
        public float rotation_y_deg;
        public float scale;
        public string material;
        public bool asset_ready;
    }

    [Serializable]
    public sealed class ZExternalManifest
    {
        public string schema;
        public string asset_format;
        public string coordinate_system;
        public ZExternalAssetEntry[] assets;
        public ZExternalPlacementEntry[] placements;
    }

    [DisallowMultipleComponent]
    public sealed class ZExternalMeshBinder : MonoBehaviour
    {
        public const string ManifestSchema = "zengine.chessboard.external-mesh-manifest/v1";
        public const uint ZbrdMagic = 0x4452425Au;

        [Tooltip("相对 StreamingAssets/<assetRoot>/ 的清单文件名")]
        public string manifestFile = "external_mesh.manifest.json";

        [Tooltip("StreamingAssets 下的资产根目录（外部 glb / zbrd 缓存 / 清单都在这）")]
        public string assetRoot = "ZEngineChessboard";

        [Tooltip("逐字节校验 glb 与网格缓存的 sha256；失败则保留代理")]
        public bool verifySha256 = true;

        [Tooltip("实例化成功后关掉同名 ZBRD 代理，避免两份几何重叠")]
        public bool hideProxyOnSuccess = true;

        public bool verbose = true;

        private readonly List<GameObject> _spawned = new List<GameObject>();
        // 贴图材质按 (asset_id, 代理材质名) 复用，避免每个实例都 clone 一份。
        private readonly Dictionary<string, Material> _materials =
            new Dictionary<string, Material>();
        private readonly List<Texture2D> _textures = new List<Texture2D>();

        internal const string MarkerName = "ExternalMeshRoot";

        private void Start()
        {
            Apply();
        }

        [ContextMenu("Rebuild External Meshes")]
        public void Apply()
        {
            foreach (GameObject spawned in _spawned)
            {
                if (spawned != null) Destroy(spawned);
            }
            _spawned.Clear();
            foreach (Material material in _materials.Values)
            {
                if (material != null) Destroy(material);
            }
            _materials.Clear();
            foreach (Texture2D texture in _textures)
            {
                if (texture != null) Destroy(texture);
            }
            _textures.Clear();

            string root = Path.Combine(Application.streamingAssetsPath, assetRoot);
            string manifestPath = Path.Combine(root, manifestFile);
            if (!File.Exists(manifestPath))
            {
                Warn("manifest not found: " + manifestPath + " (proxies kept)");
                return;
            }

            ZExternalManifest manifest;
            try
            {
                manifest = JsonUtility.FromJson<ZExternalManifest>(File.ReadAllText(manifestPath, Encoding.UTF8));
            }
            catch (Exception exception)
            {
                Warn("manifest parse failed: " + exception.Message + " (proxies kept)");
                return;
            }
            if (manifest == null || manifest.schema != ManifestSchema
                || manifest.assets == null || manifest.placements == null)
            {
                Warn("manifest schema mismatch: " + (manifest == null ? "null" : manifest.schema));
                return;
            }

            var assets = new Dictionary<string, ZExternalAssetEntry>();
            foreach (ZExternalAssetEntry asset in manifest.assets)
            {
                if (asset == null || string.IsNullOrEmpty(asset.asset_id)) continue;
                assets[asset.asset_id] = asset;
            }

            var placements = new Dictionary<string, List<ZExternalPlacementEntry>>();
            foreach (ZExternalPlacementEntry placement in manifest.placements)
            {
                if (placement == null || !placement.asset_ready) continue;
                if (!placements.TryGetValue(placement.asset_id, out List<ZExternalPlacementEntry> bucket))
                {
                    bucket = new List<ZExternalPlacementEntry>();
                    placements[placement.asset_id] = bucket;
                }
                bucket.Add(placement);
            }

            Transform artist = transform.root;
            int applied = 0;
            int failed = 0;
            foreach (KeyValuePair<string, List<ZExternalPlacementEntry>> pair in placements)
            {
                if (!assets.TryGetValue(pair.Key, out ZExternalAssetEntry asset)
                    || asset.status != "asset_ready")
                {
                    failed += pair.Value.Count;
                    Warn("asset `" + pair.Key + "` is not asset_ready (proxies kept)");
                    continue;
                }

                Mesh mesh = LoadMesh(root, asset, out Texture2D texture);
                if (mesh == null)
                {
                    failed += pair.Value.Count;
                    continue;
                }

                foreach (ZExternalPlacementEntry placement in pair.Value)
                {
                    if (Spawn(artist, mesh, texture, asset.asset_id, placement)) applied++;
                    else failed++;
                }
            }

            Log(string.Format("applied {0} instance(s), {1} kept as proxy", applied, failed));
        }

        private Mesh LoadMesh(string root, ZExternalAssetEntry asset, out Texture2D texture)
        {
            texture = null;
            string glbPath = Resolve(root, asset.mesh);
            string cachePath = Resolve(root, asset.cache);
            if (glbPath == null || cachePath == null) return null;
            if (!File.Exists(glbPath) || !File.Exists(cachePath))
            {
                Warn("asset files missing for `" + asset.asset_id + "`: " + glbPath + " / " + cachePath);
                return null;
            }
            try
            {
                // glb 是交换格式，必须真读真校验；缓存只是派生渲染数据。
                byte[] glb = File.ReadAllBytes(glbPath);
                if (verifySha256 && !Sha256Matches(glb, asset.mesh_sha256))
                {
                    Warn("glb sha256 mismatch for `" + asset.asset_id + "`");
                    return null;
                }
                byte[] cache = File.ReadAllBytes(cachePath);
                if (verifySha256 && !Sha256Matches(cache, asset.cache_sha256))
                {
                    Warn("cache sha256 mismatch for `" + asset.asset_id + "`");
                    return null;
                }
                Mesh mesh = ReadZbrd(cache, asset.pivot_offset);
                if (mesh == null) return null;
                mesh.name = asset.asset_id + "_external";
                // 贴图失败只降级成代理纯色，几何照常实例化。
                texture = LoadTexture(root, asset);
                return mesh;
            }
            catch (Exception exception)
            {
                Warn("asset `" + asset.asset_id + "` load failed: " + exception.Message);
                return null;
            }
        }

        private Texture2D LoadTexture(string root, ZExternalAssetEntry asset)
        {
            string path = Resolve(root, asset.texture);
            if (path == null || !File.Exists(path)) return null;
            try
            {
                byte[] png = File.ReadAllBytes(path);
                if (verifySha256 && !Sha256Matches(png, asset.texture_sha256))
                {
                    Warn("baseColor texture sha256 mismatch for `" + asset.asset_id + "`");
                    return null;
                }
                var texture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
                if (!ImageConversion.LoadImage(texture, png))
                {
                    Destroy(texture);
                    Warn("baseColor texture decode failed for `" + asset.asset_id + "`");
                    return null;
                }
                texture.name = asset.asset_id + "_baseColor";
                texture.wrapMode = TextureWrapMode.Repeat;
                _textures.Add(texture);
                return texture;
            }
            catch (Exception exception)
            {
                Warn("baseColor texture failed for `" + asset.asset_id + "`: "
                     + exception.Message);
                return null;
            }
        }

        private bool Spawn(Transform artist, Mesh mesh, Texture2D texture,
                           string assetId, ZExternalPlacementEntry placement)
        {
            Transform proxy = FindDeep(artist, placement.proxy_part);
            MeshRenderer proxyRenderer = proxy != null ? proxy.GetComponentInChildren<MeshRenderer>() : null;
            Material resolved = proxyRenderer != null ? proxyRenderer.sharedMaterial : null;
            if (resolved == null)
            {
                Warn("no material for `" + placement.instance_id + "` (proxy kept)");
                return false;
            }
            if (texture != null)
            {
                // 克隆代理材质而不是改 sharedMaterial，避免把贴图污染到所有代理。
                // 用项目自己的 shader（OSG_Default）保证自定义 SRP 下不空材质。
                string key = assetId + "|" + resolved.name;
                if (!_materials.TryGetValue(key, out Material textured))
                {
                    Material clone = new Material(resolved);
                    clone.name = resolved.name + "_textured";
                    SetBaseColorWhite(clone);
                    if (!SetBaseColorTexture(clone, texture))
                    {
                        Warn("no baseColor texture slot on `" + resolved.name + "`");
                        Destroy(clone);
                    }
                    else
                    {
                        _materials[key] = clone;
                        textured = clone;
                    }
                }
                if (textured != null) resolved = textured;
            }
            Vector3 anchor = ToVector(placement.anchor, Vector3.zero);
            GameObject go = new GameObject("ext_" + placement.instance_id);
            go.transform.SetParent(transform, false);
            go.transform.localPosition = anchor;
            go.transform.localRotation = Quaternion.Euler(0f, placement.rotation_y_deg, 0f);
            go.transform.localScale = Vector3.one * Mathf.Max(0.0001f, placement.scale);
            go.AddComponent<MeshFilter>().sharedMesh = mesh;
            go.AddComponent<MeshRenderer>().sharedMaterial = resolved;
            _spawned.Add(go);

            if (hideProxyOnSuccess && proxy != null) proxy.gameObject.SetActive(false);
            return true;
        }

        // ---------------- ZBRD v1 单部件读取 ----------------

        public static Mesh ReadZbrd(byte[] data, float[] pivot)
        {
            Vector3 offset = ToVector(pivot, Vector3.zero);
            using (MemoryStream stream = new MemoryStream(data, false))
            using (BinaryReader reader = new BinaryReader(stream))
            {
                if (data.Length < 16 || reader.ReadUInt32() != ZbrdMagic) return null;
                if (reader.ReadUInt32() != 1u) return null;
                int count = reader.ReadInt32();
                reader.ReadUInt32();
                Mesh mesh = null;
                for (int p = 0; p < count; p++)
                {
                    ReadString(reader);
                    reader.ReadByte();
                    reader.ReadByte();
                    reader.ReadByte();
                    reader.ReadByte();
                    ReadString(reader);
                    ReadString(reader);
                    int vertexCount = reader.ReadInt32();
                    int indexCount = reader.ReadInt32();
                    for (int i = 0; i < 16; i++) reader.ReadSingle();
                    for (int i = 0; i < 4; i++) reader.ReadSingle();

                    var vertices = new Vector3[vertexCount];
                    var normals = new Vector3[vertexCount];
                    var uvs = new Vector2[vertexCount];
                    for (int i = 0; i < vertexCount; i++)
                    {
                        vertices[i] = new Vector3(reader.ReadSingle(), reader.ReadSingle(),
                                                  reader.ReadSingle()) - offset;
                    }
                    for (int i = 0; i < vertexCount; i++)
                    {
                        normals[i] = new Vector3(reader.ReadSingle(), reader.ReadSingle(),
                                                 reader.ReadSingle());
                    }
                    for (int i = 0; i < vertexCount; i++)
                    {
                        // glTF TEXCOORD_0 是左上原点（V 向下），Unity 是左下原点；
                        // ZBRD 缓存存的是 glTF 原样 UV，消费端在这里翻 V。
                        float u = reader.ReadSingle();
                        float v = reader.ReadSingle();
                        uvs[i] = new Vector2(u, 1f - v);
                    }
                    var indices = new int[indexCount];
                    for (int i = 0; i < indexCount; i++) indices[i] = (int)reader.ReadUInt32();

                    if (mesh == null)
                    {
                        // 索引格式取决于最大索引值（顶点数上限），不是索引条数。
                        // 三角形汤时代两者恰好都 > 65000，改为索引网格后必须按
                        // vertexCount 判断，否则 2 万顶点仍被当成 UInt32。
                        mesh = new Mesh { indexFormat = vertexCount > 65535
                            ? UnityEngine.Rendering.IndexFormat.UInt32
                            : UnityEngine.Rendering.IndexFormat.UInt16 };
                        mesh.vertices = vertices;
                        mesh.normals = normals;
                        mesh.uv = uvs;
                        mesh.SetTriangles(indices, 0);
                    }
                }
                if (mesh != null) mesh.RecalculateBounds();
                return mesh;
            }
        }

        private static string ReadString(BinaryReader reader)
        {
            int length = reader.ReadInt32();
            if (length <= 0) return string.Empty;
            return Encoding.UTF8.GetString(reader.ReadBytes(length));
        }

        // ---------------- 小工具 ----------------

        // 项目用自定义 SRP（OSG_Default 等），贴图槽 / 颜色槽名字不统一，
        // 按常见顺序探测，找不到就降级为代理纯色，绝不 new Material(shader)。
        private static readonly string[] TextureSlots =
            { "_BaseMap", "_MainTex", "_AlbedoMap", "_BaseColorMap" };
        private static readonly string[] ColorSlots =
            { "_BaseColor", "_Color", "_AlbedoColor" };

        private static bool SetBaseColorTexture(Material material, Texture2D texture)
        {
            foreach (string slot in TextureSlots)
            {
                if (!material.HasProperty(slot)) continue;
                material.SetTexture(slot, texture);
                return true;
            }
            return false;
        }

        private static void SetBaseColorWhite(Material material)
        {
            // glTF: baseColor = texture × baseColorFactor，factor 恒为白色；
            // 代理纯色只作贴图缺失时的降级，否则会把贴图颜色再压暗一层。
            foreach (string slot in ColorSlots)
            {
                if (material.HasProperty(slot)) material.SetColor(slot, Color.white);
            }
        }

        private static string Resolve(string root, string relative)
        {
            if (string.IsNullOrEmpty(relative)) return null;
            return Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
        }

        private static bool Sha256Matches(byte[] data, string expected)
        {
            if (string.IsNullOrEmpty(expected)) return false;
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(data);
                var text = new StringBuilder(hash.Length * 2);
                foreach (byte value in hash) text.Append(value.ToString("x2"));
                return string.Equals(text.ToString(), expected, StringComparison.OrdinalIgnoreCase);
            }
        }

        private static Vector3 ToVector(float[] values, Vector3 fallback)
        {
            if (values == null || values.Length < 3) return fallback;
            return new Vector3(values[0], values[1], values[2]);
        }

        private static Transform FindDeep(Transform root, string name)
        {
            if (root == null || string.IsNullOrEmpty(name)) return null;
            foreach (Transform child in root.GetComponentsInChildren<Transform>(true))
            {
                if (child.name == name) return child;
            }
            return null;
        }

        private void Log(string message)
        {
            if (verbose) Debug.Log("[ZEngine][ExternalMesh] " + message, this);
        }

        private void Warn(string message)
        {
            Debug.LogWarning("[ZEngine][ExternalMesh] " + message, this);
        }
    }
}
