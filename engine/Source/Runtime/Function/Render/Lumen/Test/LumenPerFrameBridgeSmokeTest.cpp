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
// There are two real implementations and they are mutually exclusive:
//
//   Function/Render/RenderResource.cpp      -> body guarded by Z_HAS_VULKAN
//   Function/Render/RenderResourceDx12.cpp  -> body guarded by _WIN32 &&
//                                              !Z_HAS_VULKAN
//
// Apple builds Metal, so Z_HAS_VULKAN is off and neither TU compiles: there is
// no UpdatePerFrameBuffer to link at all (the Apple smoke-test stubs abort if
// reached). That is the same trap as phase 27 -- "is Lumen compiled in" is
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
static_assert(offsetof(MainCameraPerFrame, lumen_indirect) + sizeof(Vector4) ==
                  sizeof(MainCameraPerFrame),
              "lumen_indirect must be the last field of MainCameraPerFrame");
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

    // The struct mirrors must declare lumen_indirect as a 4-component vector,
    // after viewport_rect. The three copies are laid out by hand in parallel.
    void CheckStructMirror(const std::string& path, const char* vector_type)
    {
        const std::string text = ReadTextFile(path);
        if (text.empty())
        {
            ++g_Checks;
            ++g_Failures;
            std::printf("[FAIL] cannot read shader mirror: %s\n", path.c_str());
            return;
        }

        const std::size_t viewport_rect = text.find("viewport_rect");
        const std::size_t lumen = text.find("lumen_indirect");
        bool ok = true;

        if (lumen == std::string::npos)
        {
            ok = false;
            std::printf("[FAIL] %s does not declare lumen_indirect\n", path.c_str());
        }
        else
        {
            const std::size_t line_start = text.rfind('\n', lumen);
            const std::size_t line_end = text.find('\n', lumen);
            const std::string line = text.substr(line_start == std::string::npos ? 0 : line_start,
                                                 line_end == std::string::npos
                                                     ? std::string::npos
                                                     : line_end - line_start);
            if (!Contains(line, vector_type))
            {
                ok = false;
                std::printf("[FAIL] %s declares lumen_indirect without %s\n", path.c_str(), vector_type);
            }
        }

        if (viewport_rect == std::string::npos || lumen == std::string::npos || lumen < viewport_rect)
        {
            ok = false;
            std::printf("[FAIL] %s declares lumen_indirect before viewport_rect\n", path.c_str());
        }

        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %s mirrors lumen_indirect as %s after viewport_rect\n", path.c_str(),
                        vector_type);
        }
        else
        {
            ++g_Failures;
        }
    }

    // A consumer must BLEND with the weight, not add to the baked irradiance --
    // adding would count the sky twice.
    void CheckConsumer(const std::string& path, const char* blend_function)
    {
        const std::string text = ReadTextFile(path);
        const std::size_t use = text.find("lumen_indirect.rgb");
        bool ok = use != std::string::npos && Contains(text, "lumen_indirect.a");

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
                std::printf("[FAIL] %s uses lumen_indirect without %s (adding it would double-count "
                            "the baked sky)\n",
                            path.c_str(), blend_function);
            }
        }
        else
        {
            std::printf("[FAIL] %s does not consume lumen_indirect\n", path.c_str());
        }

        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %s blends lumen_indirect with %s\n", path.c_str(), blend_function);
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
        Check(SameVector4(fresh.ToUniform(), Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
              "a fresh scene packs to (0,0,0,0)");
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
    }

    // -------------------------------------------------------------------------
    // 3. The layout contract, spelled out at runtime so it shows up in the
    //    output as well as at compile time.
    // -------------------------------------------------------------------------
    Check(offsetof(MainCameraPerFrame, lumen_indirect) ==
              offsetof(MainCameraPerFrame, viewport_rect) + sizeof(Vector4),
          "lumen_indirect directly follows viewport_rect in C++");
    Check(offsetof(MainCameraPerFrame, lumen_indirect) + sizeof(Vector4) == sizeof(MainCameraPerFrame),
          "lumen_indirect is the last field of MainCameraPerFrame");
    Check(sizeof(MainCameraPerFrame) % 16 == 0, "MainCameraPerFrame stays 16-byte aligned");

    // -------------------------------------------------------------------------
    // 4. Call sites + shader mirrors. Only compiled in when CMake hands us the
    //    engine root (see Function/Render/Lumen/Test/CMakeLists.txt).
    // -------------------------------------------------------------------------
#if defined(ZENGINE_ENGINE_ROOT_DIR)
    const std::string root = ZENGINE_ENGINE_ROOT_DIR;
    const std::string render_dir = root + "/Source/Runtime/Function/Render";

    // Both backend implementations must publish; neither compiles on this host.
    CheckSource(render_dir + "/RenderResource.cpp", "lumen_indirect",
                "the Vulkan UpdatePerFrameBuffer publishes lumen_indirect");
    CheckSource(render_dir + "/RenderResourceDx12.cpp", "lumen_indirect",
                "the DX12 UpdatePerFrameBuffer publishes lumen_indirect");

    const std::string shader = root + "/shader";
    CheckStructMirror(shader + "/include/structures.h", "vec4");
    CheckStructMirror(shader + "/hlsl/common/scene_lighting_structs.hlsli", "float4");
    CheckSource(shader + "/include/main_camera_per_frame_access.inl",
                "#define lumen_indirect per_frame.lumen_indirect",
                "the GLSL bare-name accessor exposes lumen_indirect");
    CheckSource(shader + "/hlsl/common/main_camera_per_frame_access.hlsli",
                "#define lumen_indirect per_frame.lumen_indirect",
                "the HLSL bare-name accessor exposes lumen_indirect");
    CheckConsumer(shader + "/include/mesh_lighting.inl", "mix(");
    CheckConsumer(shader + "/hlsl/rp1/deferred_lighting.frag.hlsl", "lerp(");
#endif

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
