// =============================================================================
// Lumen -> per-frame uniform bridge smoke test.
//
// Phase 27 made LumenRenderPass publish its irradiance, but "published" only
// meant "written into RenderScene::m_IndirectLighting". Nothing carried it the
// last metre into the shader, so GI still could not reach a pixel. Phase 28
// added MainCameraPerFrame::lumen_indirect and a copy in every
// UpdatePerFrameBuffer -- that copy is the ONLY path from the CPU probe grid to
// the screen, and this test pins it.
//
// Why this test does not simply call UpdatePerFrameBuffer
// ------------------------------------------------------
// There is now ONE implementation, Function/Render/RenderResourcePerFrame.cpp,
// compiled on every platform. It used to be two mutually exclusive copies:
//
//   RenderResource.cpp     -> body guarded by Z_HAS_VULKAN
//   RenderResourceDx12.cpp -> body guarded by _WIN32 && !Z_HAS_VULKAN
//
// so on Apple (Metal, Z_HAS_VULKAN off) there was no UpdatePerFrameBuffer to
// link at all -- the Apple smoke-test stubs aborted if reached. The one copy
// also means the two variants can no longer drift apart. That is the same trap as phase 27 -- "is Lumen compiled in" is
// Z_HAS_LUMEN, not Z_HAS_VULKAN -- so a test that called it would only ever run
// on Vulkan hosts.
//
// The packing therefore lives in SceneIndirectLighting::ToUniform(), which is
// what both backends call. This test pins:
//
//   * ToUniform(): rgb = irradiance, a = weight, and a black scene with weight 1
//     still publishes weight 1 -- "black is data, not absence" is the whole
//     reason the bridge carries weight + colour.
//   * The C++ struct layout that the three hand-maintained mirrors (C++, GLSL
//     structures.h, HLSL scene_lighting_structs.hlsli) must agree on. A field
//     added to only one of them shifts every later per-frame constant by 16
//     bytes, and the shaders are never compiled on this host, so nothing else
//     would catch it.
//   * That both backend call sites actually reference lumen_indirect (source
//     check -- the only way to guard a Vulkan-only / Windows-only line from
//     macOS), and that both shader consumers BLEND instead of adding (adding
//     would count the baked sky irradiance twice).
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_PER_FRAME_BRIDGE_SMOKE_TEST=ON
//   cmake --build build --target LumenPerFrameBridgeSmokeTest
// =============================================================================

#include "Runtime/Function/Render/RenderCommon.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// -----------------------------------------------------------------------------
// Layout contract (the C++ half of the three-way mirror).
// -----------------------------------------------------------------------------
static_assert(offsetof(MainCameraPerFrame, lumen_indirect) ==
                  offsetof(MainCameraPerFrame, viewport_rect) + sizeof(Vector4),
              "lumen_indirect must directly follow viewport_rect (matches the GLSL/HLSL mirrors)");
static_assert(offsetof(MainCameraPerFrame, lumen_reflection) ==
                  offsetof(MainCameraPerFrame, lumen_indirect) + sizeof(Vector4),
              "lumen_reflection must directly follow lumen_indirect");
// CSM 的字段接在 lumen_* 之后（阶段 71）。这条链是"三份镜像逐字节一致"的 C++
// 半边：任何人往中间插字段、或者改了字段顺序而没同步 shader 侧，这里会先炸。
static_assert(offsetof(MainCameraPerFrame, camera_forward) ==
                  offsetof(MainCameraPerFrame, lumen_reflection) + sizeof(Vector4),
              "camera_forward must directly follow lumen_reflection");
static_assert(offsetof(MainCameraPerFrame, directional_light_proj_view_cascades) ==
                  offsetof(MainCameraPerFrame, camera_forward) + sizeof(Vector4),
              "the cascade matrices must directly follow camera_forward (+ its padding)");
static_assert(offsetof(MainCameraPerFrame, directional_light_cascade_split_depth) ==
                  offsetof(MainCameraPerFrame, directional_light_proj_view_cascades) +
                      s_MaxCascadeCount * sizeof(Matrix4x4),
              "the split depths must directly follow the cascade matrices");
static_assert(offsetof(MainCameraPerFrame, directional_light_cascade_count) ==
                  offsetof(MainCameraPerFrame, directional_light_cascade_split_depth) + sizeof(Vector4),
              "the cascade count must directly follow the split depths");
static_assert(offsetof(MainCameraPerFrame, directional_light_cascade_count) + 4 * sizeof(float) ==
                  sizeof(MainCameraPerFrame),
              "the cascade count block must be the last field of MainCameraPerFrame");
static_assert(sizeof(MainCameraPerFrame) % 16 == 0,
              "MainCameraPerFrame must stay 16-byte aligned (it is a uniform/storage buffer)");

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    bool NearlyEqual(float a, float b) { return std::fabs(a - b) < 1e-5f; }

    bool SameVector4(const Vector4& a, const Vector4& b)
    {
        return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y) && NearlyEqual(a.z, b.z) &&
               NearlyEqual(a.w, b.w);
    }

    std::string ReadTextFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::string();
        }
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    bool Contains(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    // The source-text checks below are crude on purpose: they guard lines this
    // host cannot compile (Vulkan-only / Windows-only C++, and every shader).
    // A missing file is a failure, not a skip -- otherwise a moved file would
    // silently disable the guard.
    void CheckSource(const std::string& path, const char* what, const char* name)
    {
        const std::string text = ReadTextFile(path);
        const bool ok = !text.empty() && Contains(text, what);
        ++g_Checks;
        if (!ok)
        {
            ++g_Failures;
            std::printf("[FAIL] %s (%s missing from %s)\n", name, what, path.c_str());
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    // The struct mirrors must declare the field as a 4-component vector, after
    // the given anchor. The three copies are laid out by hand in parallel.
    void CheckStructMirror(const std::string& path, const char* field, const char* anchor,
                           const char* vector_type)
    {
        const std::string text = ReadTextFile(path);
        if (text.empty())
        {
            ++g_Checks;
            ++g_Failures;
            std::printf("[FAIL] cannot read shader mirror: %s\n", path.c_str());
            return;
        }

        const std::size_t anchor_at = text.find(anchor);
        const std::size_t field_at = text.find(field);
        bool ok = true;

        if (field_at == std::string::npos)
        {
            ok = false;
            std::printf("[FAIL] %s does not declare %s\n", path.c_str(), field);
        }
        else
        {
            const std::size_t line_start = text.rfind('\n', field_at);
            const std::size_t line_end = text.find('\n', field_at);
            const std::string line = text.substr(line_start == std::string::npos ? 0 : line_start,
                                                 line_end == std::string::npos
                                                     ? std::string::npos
                                                     : line_end - line_start);
            if (!Contains(line, vector_type))
            {
                ok = false;
                std::printf("[FAIL] %s declares %s without %s\n", path.c_str(), field, vector_type);
            }
        }

        if (anchor_at == std::string::npos || field_at == std::string::npos || field_at < anchor_at)
        {
            ok = false;
            std::printf("[FAIL] %s declares %s before %s\n", path.c_str(), field, anchor);
        }

        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %s mirrors %s as %s after %s\n", path.c_str(), field, vector_type, anchor);
        }
        else
        {
            ++g_Failures;
        }
    }

    // A consumer must BLEND with the weight, not add to the baked irradiance --
    // adding would count the sky twice.
    void CheckConsumer(const std::string& path, const char* field, const char* blend_function)
    {
        const std::string text = ReadTextFile(path);
        const std::string rgb_access = std::string(field) + ".rgb";
        const std::string a_access = std::string(field) + ".a";
        const std::size_t use = text.find(rgb_access);
        bool ok = use != std::string::npos && Contains(text, a_access.c_str());

        if (ok)
        {
            const std::size_t line_start = text.rfind('\n', use);
            const std::size_t line_end = text.find('\n', use);
            const std::string line = text.substr(line_start == std::string::npos ? 0 : line_start,
                                                 line_end == std::string::npos
                                                     ? std::string::npos
                                                     : line_end - line_start);
            ok = Contains(line, blend_function);
            if (!ok)
            {
                std::printf("[FAIL] %s uses %s without %s (adding it would double-count the baked "
                            "environment)\n",
                            path.c_str(), field, blend_function);
            }
        }
        else
        {
            std::printf("[FAIL] %s does not consume %s\n", path.c_str(), field);
        }

        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %s blends %s with %s\n", path.c_str(), field, blend_function);
        }
        else
        {
            ++g_Failures;
        }
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // 0. Defaults: a scene that has never seen Lumen publishes weight 0, which
    //    is what tells the shader to keep using the baked irradiance cubemap
    //    (i.e. to behave exactly as it did before Lumen existed).
    // -------------------------------------------------------------------------
    {
        SceneIndirectLighting fresh {};
        Check(fresh.weight == 0.0f, "SceneIndirectLighting defaults to weight 0");
        Check(fresh.irradiance == Vector3::ZERO, "SceneIndirectLighting defaults to black");
        Check(fresh.frame_index == 0, "SceneIndirectLighting defaults to frame 0");
        Check(fresh.reflection == Vector3::ZERO, "SceneIndirectLighting defaults to no reflection");
        Check(fresh.reflection_weight == 0.0f, "SceneIndirectLighting defaults to reflection weight 0");
        Check(SameVector4(fresh.ToUniform(), Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
              "a fresh scene packs to (0,0,0,0)");
        Check(SameVector4(fresh.ToReflectionUniform(), Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
              "a fresh scene packs reflection to (0,0,0,0)");
    }

    // -------------------------------------------------------------------------
    // 1. The packing: rgb = irradiance, a = weight. Both backends call this, so
    //    it is the single definition of what reaches the shader.
    // -------------------------------------------------------------------------
    {
        SceneIndirectLighting red_bounce {};
        red_bounce.irradiance = Vector3(0.4f, 0.1f, 0.05f);
        red_bounce.weight = 1.0f;
        red_bounce.frame_index = 7;

        const Vector4 packed = red_bounce.ToUniform();
        Check(NearlyEqual(packed.x, 0.4f) && NearlyEqual(packed.y, 0.1f) && NearlyEqual(packed.z, 0.05f),
              "the irradiance channels pack in order (rgb)");
        Check(NearlyEqual(packed.w, 1.0f), "the weight packs into .a");
        Check(red_bounce.frame_index == 7, "the frame index is NOT packed (only irradiance + weight reach the GPU)");

        // GI 和反射是两套独立的权重（r.Lumen.GlobalIllumination 与 r.Lumen.Reflections
        // 是两个 CVar），所以各自的打包互不干扰：GI 关掉不该把反射也关掉。
        red_bounce.reflection = Vector3(0.2f, 0.3f, 0.4f);
        red_bounce.reflection_weight = 1.0f;
        const Vector4 packed_reflection = red_bounce.ToReflectionUniform();
        Check(NearlyEqual(packed_reflection.x, 0.2f) && NearlyEqual(packed_reflection.y, 0.3f) &&
                  NearlyEqual(packed_reflection.z, 0.4f),
              "the reflection channels pack in order (rgb)");
        Check(NearlyEqual(packed_reflection.w, 1.0f), "the reflection weight packs into .a");
        Check(SameVector4(red_bounce.ToUniform(), Vector4(0.4f, 0.1f, 0.05f, 1.0f)),
              "packing the reflection leaves the GI packing untouched");
    }

    // -------------------------------------------------------------------------
    // 2. Weight 0 vs. black. These must be distinguishable, and only the weight
    //    distinguishes them -- an enclosed scene legitimately resolves to black.
    // -------------------------------------------------------------------------
    {
        SceneIndirectLighting enclosed {};
        enclosed.irradiance = Vector3::ZERO;
        enclosed.weight = 1.0f;
        Check(SameVector4(enclosed.ToUniform(), Vector4(0.0f, 0.0f, 0.0f, 1.0f)),
              "a black scene with weight 1 still packs weight 1 (black is data, not absence)");

        SceneIndirectLighting off {};
        off.irradiance = Vector3(0.4f, 0.1f, 0.05f);
        off.weight = 0.0f;
        Check(NearlyEqual(off.ToUniform().w, 0.0f),
              "weight 0 stays weight 0 no matter what colour is left over");

        // 关掉反射而 GI 还开着：反射权重必须归零，GI 权重必须还是 1
        SceneIndirectLighting gi_only {};
        gi_only.irradiance = Vector3(0.4f, 0.1f, 0.05f);
        gi_only.weight = 1.0f;
        gi_only.reflection_weight = 0.0f;
        Check(NearlyEqual(gi_only.ToReflectionUniform().w, 0.0f),
              "GI on + reflections off packs reflection weight 0");
        Check(NearlyEqual(gi_only.ToUniform().w, 1.0f), "...and leaves the GI weight at 1");
    }

    // -------------------------------------------------------------------------
    // 3. The layout contract, spelled out at runtime so it shows up in the
    //    output as well as at compile time.
    // -------------------------------------------------------------------------
    Check(offsetof(MainCameraPerFrame, lumen_indirect) ==
              offsetof(MainCameraPerFrame, viewport_rect) + sizeof(Vector4),
          "lumen_indirect directly follows viewport_rect in C++");
    Check(offsetof(MainCameraPerFrame, lumen_reflection) ==
              offsetof(MainCameraPerFrame, lumen_indirect) + sizeof(Vector4),
          "lumen_reflection directly follows lumen_indirect in C++");
    Check(offsetof(MainCameraPerFrame, directional_light_cascade_count) + 4 * sizeof(float) ==
              sizeof(MainCameraPerFrame),
          "the cascade count block is the last field of MainCameraPerFrame (CSM fields are appended "
          "after lumen_reflection)");
    Check(sizeof(MainCameraPerFrame) % 16 == 0, "MainCameraPerFrame stays 16-byte aligned");

    // -------------------------------------------------------------------------
    // 4. Call sites + shader mirrors. Only compiled in when CMake hands us the
    //    engine root (see Function/Render/Lumen/Test/CMakeLists.txt).
    // -------------------------------------------------------------------------
#if defined(ZENGINE_ENGINE_ROOT_DIR)
    const std::string root = ZENGINE_ENGINE_ROOT_DIR;
    const std::string render_dir = root + "/Source/Runtime/Function/Render";

    // 只有一份实现（RenderResourcePerFrame.cpp），Vulkan / DX12 / Metal 三个后端
    // 共用，所以查一个文件就等于查全部。以前这里要分别查两个后端文件。
    CheckSource(render_dir + "/RenderResourcePerFrame.cpp", "lumen_indirect",
                "UpdatePerFrameBuffer publishes lumen_indirect");
    CheckSource(render_dir + "/RenderResourcePerFrame.cpp", "lumen_reflection",
                "UpdatePerFrameBuffer publishes lumen_reflection");

    const std::string shader = root + "/shader";
    CheckStructMirror(shader + "/include/structures.h", "lumen_indirect", "viewport_rect", "vec4");
    CheckStructMirror(shader + "/include/structures.h", "lumen_reflection", "lumen_indirect", "vec4");
    CheckStructMirror(shader + "/hlsl/common/scene_lighting_structs.hlsli", "lumen_indirect",
                      "viewport_rect", "float4");
    CheckStructMirror(shader + "/hlsl/common/scene_lighting_structs.hlsli", "lumen_reflection",
                      "lumen_indirect", "float4");
    CheckSource(shader + "/include/main_camera_per_frame_access.inl",
                "#define lumen_indirect per_frame.lumen_indirect",
                "the GLSL bare-name accessor exposes lumen_indirect");
    CheckSource(shader + "/include/main_camera_per_frame_access.inl",
                "#define lumen_reflection per_frame.lumen_reflection",
                "the GLSL bare-name accessor exposes lumen_reflection");
    CheckSource(shader + "/hlsl/common/main_camera_per_frame_access.hlsli",
                "#define lumen_indirect per_frame.lumen_indirect",
                "the HLSL bare-name accessor exposes lumen_indirect");
    CheckSource(shader + "/hlsl/common/main_camera_per_frame_access.hlsli",
                "#define lumen_reflection per_frame.lumen_reflection",
                "the HLSL bare-name accessor exposes lumen_reflection");
    CheckConsumer(shader + "/include/mesh_lighting.inl", "lumen_indirect", "mix(");
    CheckConsumer(shader + "/include/mesh_lighting.inl", "lumen_reflection", "mix(");
    // 延迟通道（HLSL）没有 specular IBL 项可以混，所以它只消费 GI。
    CheckConsumer(shader + "/hlsl/rp1/deferred_lighting.frag.hlsl", "lumen_indirect", "lerp(");
#endif

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
