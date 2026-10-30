// =============================================================================
// glTF / GLB 几何导入冒烟测试。
//
//   cmake -B build -DZENGINE_BUILD_GLTF_MESH_IMPORT_SMOKE_TEST=ON
//   cmake --build build --target GltfMeshImportSmokeTest
//   ctest --test-dir build -C Debug -R GltfMeshImportSmokeTest
//
// 为什么写它
// ----------
// 在 2026-10-30 之前，`CanImport` 无条件列出 `.gltf` / `.glb`，而 `Import()`
// 里对应分支只有一句 `error = "glTF/GLB import is not implemented yet"`。
// 也就是 ContentBrowser 里能选、能点导入、然后**必然失败** —— 用户分不清
// "这引擎不支持"和"我的文件坏了"。这就是"假支持"。
//
// 现在真支持了，风险从"必然失败"变成了更糟的一种：**导入成功但几何是错的**。
// glTF 是个大格式，部分支持最容易退化成静默错误。所以这里钉的是：
//
//   A. .gltf（外置 .bin）/ .gltf（data: URI）/ .glb 三条路径都能出几何，
//      且**互相一致**（同一份数据三种容器，结果必须逐字节相同）。
//   B. 属性正确：位置受 scale、法线、UV、索引。
//   C. 节点世界变换生效，且**法线用逆转置** —— 非均匀缩放下用原矩阵变换法线
//      会得到"看着导入成功、光照全错"的模型。这一组专门设计成能区分两者。
//   D. 不支持的东西**显式报错**，不是静默产出错误几何：LINES 图元、蒙皮、
//      无几何、坏文件。
//   E. 源码级：扩展名清单必须由解析器宏门控（防"假支持"复发）。
// =============================================================================

#include "Editor/AssetPipeline/MeshImporter/GltfMeshReader.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    void CheckEq(long long got, long long want, const char* id, const char* what)
    {
        ++g_Checks;
        if (got == want)
        {
            std::printf("[ ok ] %-4s %s (=%lld)\n", id, what, got);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s got=%lld want=%lld\n", id, what, got, want);
        }
    }

    void CheckNear(float got, float want, const char* id, const char* what, float tol = 1.0e-5f)
    {
        ++g_Checks;
        if (std::fabs(got - want) <= tol)
        {
            std::printf("[ ok ] %-4s %s (=%.6f)\n", id, what, static_cast<double>(got));
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s got=%.6f want=%.6f\n", id, what,
                        static_cast<double>(got), static_cast<double>(want));
        }
    }

    bool Contains(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    // ---- 夹具构造 ----

    void AppendU32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }

    void AppendFloats(std::vector<uint8_t>& out, const std::vector<float>& values)
    {
        for (float v : values)
        {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            out.insert(out.end(), b, b + 4);
        }
    }

    void AppendU16(std::vector<uint8_t>& out, const std::vector<uint16_t>& values)
    {
        for (uint16_t v : values)
        {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        }
    }

    /// 一个三角形的顶点数据。三个容器（.bin / data: URI / GLB）共用同一份，
    /// 这样 A 组的"三种容器结果一致"才有意义。
    ///
    /// 布局（每段 4 字节对齐）：
    ///   0..35   POSITION  (3 x VEC3 float)
    ///   36..71  NORMAL    (3 x VEC3 float)
    ///   72..95  TEXCOORD  (3 x VEC2 float)
    ///   96..101 INDICES   (3 x uint16)  -> 补齐到 104
    std::vector<uint8_t> MakeTriangleBuffer()
    {
        std::vector<uint8_t> buf;
        AppendFloats(buf, {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f});
        AppendFloats(buf, {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f});
        AppendFloats(buf, {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f});
        AppendU16(buf, {0, 1, 2});
        while (buf.size() % 4 != 0)
        {
            buf.push_back(0);
        }
        return buf;
    }

    std::string Base64(const std::vector<uint8_t>& bytes)
    {
        static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            const uint32_t b0 = bytes[i];
            const uint32_t b1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0u;
            const uint32_t b2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0u;
            const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            out.push_back(kTable[(triple >> 18) & 0x3F]);
            out.push_back(kTable[(triple >> 12) & 0x3F]);
            out.push_back((i + 1 < bytes.size()) ? kTable[(triple >> 6) & 0x3F] : '=');
            out.push_back((i + 2 < bytes.size()) ? kTable[triple & 0x3F] : '=');
        }
        return out;
    }

    /// bufferViews / accessors 三份容器共用，只有 buffers[0].uri 不同。
    const char* kTriangleJsonTail = R"JSON(
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 96, "byteLength": 6  }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3 } ] } ],
  "nodes": [ { "mesh": 0 } ],
  "scenes": [ { "nodes": [ 0 ] } ],
  "scene": 0
)JSON";

    std::string MakeTriangleJson(const std::string& buffer_uri, size_t byte_length)
    {
        // ⚠️ 不能用固定大小的 snprintf 缓冲：data: URI 的 base64 有几百字节，
        // 截断出来的 JSON 是"能写盘、解析必失败"的假夹具 —— 症状看起来像
        // cgltf 的 bug，实际是夹具自己的。
        std::string head = R"JSON({ "asset": { "version": "2.0" }, "buffers": [ { "byteLength": )JSON"
                         + std::to_string(byte_length)
                         + R"JSON(, "uri": ")JSON" + buffer_uri + R"JSON(" } ],)JSON";
        return head + kTriangleJsonTail + "}";
    }

    void WriteBytes(const std::filesystem::path& p, const std::vector<uint8_t>& bytes)
    {
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const std::filesystem::path& p, const std::string& text)
    {
        std::ofstream out(p, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    /// 手搓 GLB：12 字节头 + JSON chunk + BIN chunk。
    std::vector<uint8_t> MakeGlb(const std::string& json, const std::vector<uint8_t>& bin)
    {
        std::vector<uint8_t> json_chunk(json.begin(), json.end());
        while (json_chunk.size() % 4 != 0)
        {
            json_chunk.push_back(0x20);  // JSON chunk 补空格（规范要求）
        }
        std::vector<uint8_t> bin_chunk = bin;
        while (bin_chunk.size() % 4 != 0)
        {
            bin_chunk.push_back(0x00);   // BIN chunk 补零
        }

        const uint32_t total = static_cast<uint32_t>(12 + 8 + json_chunk.size() + 8 + bin_chunk.size());
        std::vector<uint8_t> out;
        AppendU32(out, 0x46546C67u);  // "glTF"
        AppendU32(out, 2u);
        AppendU32(out, total);

        AppendU32(out, static_cast<uint32_t>(json_chunk.size()));
        AppendU32(out, 0x4E4F534Au);  // "JSON"
        out.insert(out.end(), json_chunk.begin(), json_chunk.end());

        AppendU32(out, static_cast<uint32_t>(bin_chunk.size()));
        AppendU32(out, 0x004E4942u);  // "BIN\0"
        out.insert(out.end(), bin_chunk.begin(), bin_chunk.end());
        return out;
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\n=== GltfMeshImport smoke test ===\n\n");

    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "zengine_gltf_mesh_import_test";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    const std::vector<uint8_t> tri_bin = MakeTriangleBuffer();

    // -------------------------------------------------------------------------
    // A. 三种容器（.gltf+外置 .bin / .gltf+data: URI / .glb）必须给出同一份几何
    // -------------------------------------------------------------------------
    struct Case { const char* name; std::string path; };
    std::vector<Case> cases;

    {
        const std::filesystem::path p = scratch / "external.gltf";
        WriteBytes(scratch / "external.bin", tri_bin);
        WriteText(p, MakeTriangleJson("external.bin", tri_bin.size()));
        cases.push_back({"gltf + external .bin", p.generic_string()});
    }
    {
        const std::filesystem::path p = scratch / "embedded.gltf";
        const std::string uri = "data:application/octet-stream;base64," + Base64(tri_bin);
        WriteText(p, MakeTriangleJson(uri, tri_bin.size()));
        cases.push_back({"gltf + data: URI", p.generic_string()});
    }
    {
        const std::filesystem::path p = scratch / "tri.glb";
        // GLB 的 BIN chunk 就是 buffer 0，uri 省略。
        const std::string json = std::string(R"JSON({ "asset": { "version": "2.0" }, "buffers": [ { "byteLength": )JSON")
                               + std::to_string(tri_bin.size()) + " } ]," + kTriangleJsonTail + "}";
        WriteBytes(p, MakeGlb(json, tri_bin));
        cases.push_back({"glb", p.generic_string()});
    }

    // MeshData 是 Object，拷贝赋值被删 —— 只留数组做跨容器比对。
    std::vector<Vertex> reference_vertices;
    std::vector<int>    reference_indices;
    for (const Case& c : cases)
    {
        std::printf("-- A. %s --\n", c.name);
        MeshData mesh;
        std::string err;
        std::vector<std::string> warns;
        const bool ok = zgltf::ReadGltfMeshGeometry(c.path, 1.0f, mesh, err, warns);
        Check(ok, "A1", (std::string(c.name) + ": import succeeded").c_str());
        if (!ok)
        {
            std::printf("       error: %s\n", err.c_str());
            continue;
        }
        CheckEq(static_cast<long long>(mesh.vertex_buffer.size()), 3, "A2", "3 vertices");
        CheckEq(static_cast<long long>(mesh.index_buffer.size()), 3, "A3", "3 indices");

        if (reference_vertices.empty())
        {
            reference_vertices = mesh.vertex_buffer;
            reference_indices  = mesh.index_buffer;
        }
        else
        {
            bool same = mesh.vertex_buffer.size() == reference_vertices.size();
            for (size_t i = 0; same && i < mesh.vertex_buffer.size(); ++i)
            {
                same = std::memcmp(&mesh.vertex_buffer[i], &reference_vertices[i], sizeof(Vertex)) == 0;
            }
            Check(same, "A4", "geometry is byte-identical across containers");
        }
    }

    // -------------------------------------------------------------------------
    // B. 属性正确（拿 A 组的第一份结果）
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- B. 属性 --\n");
        Check(reference_vertices.size() == 3, "B0", "reference geometry present");
        if (reference_vertices.size() == 3)
        {
            const Vertex& v0 = reference_vertices[0];
            const Vertex& v1 = reference_vertices[1];
            const Vertex& v2 = reference_vertices[2];
            CheckNear(v0.px, 0.0f, "B1", "v0.px");
            CheckNear(v1.px, 1.0f, "B2", "v1.px == 1");
            CheckNear(v2.py, 1.0f, "B3", "v2.py == 1");
            CheckNear(v1.nz, 1.0f, "B4", "normal.z == 1");
            CheckNear(v1.u, 1.0f, "B5", "v1.u == 1");
            CheckNear(v2.v, 1.0f, "B6", "v2.v == 1");
            CheckEq(reference_indices.empty() ? -1 : reference_indices[2], 2, "B7", "index[2] == 2");
        }
    }

    // scale 必须只作用于位置（与 OBJ 导入一致）
    {
        std::printf("\n-- B8. scale --\n");
        MeshData mesh;
        std::string err;
        std::vector<std::string> warns;
        if (zgltf::ReadGltfMeshGeometry(cases.front().path, 2.0f, mesh, err, warns) &&
            mesh.vertex_buffer.size() == 3)
        {
            CheckNear(mesh.vertex_buffer[1].px, 2.0f, "B8", "scale 2 doubles positions");
            CheckNear(mesh.vertex_buffer[1].nz, 1.0f, "B9", "scale does NOT scale normals");
        }
        else
        {
            Check(false, "B8", "scaled import failed");
        }
    }

    // -------------------------------------------------------------------------
    // C. 节点世界变换 + 法线逆转置（非均匀缩放）
    //
    //     节点 scale = (1, 2, 1)，法线取 (1,1,0)/√2：
    //       逆转置（正确）: diag(1, 0.5, 1) · n ∝ (0.7071, 0.3536, 0) -> (0.8944, 0.4472, 0)
    //       原矩阵（错误）: diag(1, 2, 1) · n   ∝ (0.7071, 1.4142, 0) -> (0.4472, 0.8944, 0)
    //     两个结果差得足够远，1e-3 的容差就能区分 —— 用错矩阵一定红。
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- C. 非均匀缩放下的法线 --\n");

        const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const float k = 0.70710678f;  // 1/√2
        const std::vector<float> normals = {k, k, 0.0f, k, k, 0.0f, k, k, 0.0f};

        std::vector<uint8_t> buf;
        AppendFloats(buf, positions);
        AppendFloats(buf, normals);
        const size_t bin_len = buf.size();

        const std::string json =
            std::string(R"JSON({ "asset": { "version": "2.0" }, "buffers": [ { "byteLength": )JSON")
            + std::to_string(bin_len) + " } ],"
            + R"JSON(
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" }
  ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "NORMAL": 1 } } ] } ],
  "nodes": [ { "mesh": 0, "scale": [1, 2, 1] } ],
  "scenes": [ { "nodes": [ 0 ] } ],
  "scene": 0 })JSON";

        const std::filesystem::path p = scratch / "scaled.glb";
        WriteBytes(p, MakeGlb(json, buf));

        MeshData mesh;
        std::string err;
        std::vector<std::string> warns;
        const bool ok = zgltf::ReadGltfMeshGeometry(p, 1.0f, mesh, err, warns);
        Check(ok, "C1", "scaled mesh imported");
        if (ok && mesh.vertex_buffer.size() == 3)
        {
            CheckNear(mesh.vertex_buffer[2].py, 2.0f, "C2", "node scale (1,2,1) applied to position");
            // 0.4472 = 用原矩阵（错）的结果；0.8944 = 用逆转置（对）的结果。
            CheckNear(mesh.vertex_buffer[0].nx, 0.894427f, "C3",
                      "normal uses inverse-transpose (not the raw matrix)", 1.0e-3f);
            CheckNear(mesh.vertex_buffer[0].ny, 0.447214f, "C4",
                      "inverse-transposed normal.y", 1.0e-3f);
        }
    }

    // -------------------------------------------------------------------------
    // D. 不支持的东西必须报错，不是静默产出错误几何
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- D. 显式报错 --\n");

        auto expect_failure = [&](const char* id, const std::string& json,
                                  const std::vector<uint8_t>& bin, const char* needle, const char* what)
        {
            const std::filesystem::path p = scratch / (std::string("bad_") + id + ".glb");
            WriteBytes(p, MakeGlb(json, bin));
            MeshData mesh;
            std::string err;
            std::vector<std::string> warns;
            const bool ok = zgltf::ReadGltfMeshGeometry(p, 1.0f, mesh, err, warns);
            Check(!ok, id, what);
            if (!ok)
            {
                std::printf("       error: %s\n", err.c_str());
                Check(Contains(err, needle), (std::string(id) + "m").c_str(), "error names the reason");
            }
        };

        // LINES 图元
        expect_failure("D1",
            R"JSON({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],
              "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
              "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
              "meshes":[{"primitives":[{"mode":1,"attributes":{"POSITION":0}}]}],
              "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})JSON",
            tri_bin, "unsupported primitive mode", "LINES primitive is refused (not silently imported)");

        // 蒙皮
        expect_failure("D3",
            R"JSON({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],
              "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
              "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
              "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
              "nodes":[{"mesh":0,"skin":0},{"mesh":0}],
              "skins":[{"joints":[1]}],
              "scenes":[{"nodes":[0]}],"scene":0})JSON",
            tri_bin, "skinned", "skinned mesh is refused (not imported as static)");

        // 有 nodes 但没有 mesh
        expect_failure("D5",
            R"JSON({"asset":{"version":"2.0"},"nodes":[{}],"scenes":[{"nodes":[0]}],"scene":0})JSON",
            {}, "no triangle geometry", "empty scene is refused");

        // 坏文件
        {
            const std::filesystem::path p = scratch / "garbage.gltf";
            WriteText(p, "this is not glTF at all");
            MeshData mesh;
            std::string err;
            std::vector<std::string> warns;
            Check(!zgltf::ReadGltfMeshGeometry(p, 1.0f, mesh, err, warns), "D7",
                  "garbage file is refused");
            std::printf("       error: %s\n", err.c_str());
        }

        // 不存在的文件
        {
            MeshData mesh;
            std::string err;
            std::vector<std::string> warns;
            Check(!zgltf::ReadGltfMeshGeometry(scratch / "does_not_exist.gltf", 1.0f, mesh, err, warns),
                  "D8", "missing file is refused");
        }
    }

    // -------------------------------------------------------------------------
    // E. 源码级：扩展名清单必须由解析器宏门控（防"假支持"复发）
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- E. 假支持回归门 --\n");
        const std::string src_path =
            std::string(ZENGINE_ENGINE_ROOT_DIR) +
            "/Source/Editor/AssetPipeline/MeshImporter/MeshImporter.cpp";
        std::ifstream in(src_path);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Check(!src.empty(), "E1", "can read MeshImporter.cpp");
        if (!src.empty())
        {
            Check(!Contains(src, "not implemented yet"), "E2",
                  "no 'not implemented yet' branch left");
            Check(Contains(src, "ZENGINE_HAS_CGLTF"), "E3",
                  "glTF path is gated by ZENGINE_HAS_CGLTF");
            Check(Contains(src, "ZENGINE_HAS_UFBX"), "E4",
                  "FBX path is gated by ZENGINE_HAS_UFBX");
        }
    }

    std::filesystem::remove_all(scratch, ec);

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
