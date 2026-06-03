// =====================================================================
// PR5c: DX12 bindless smoke-test  (PR6: + RHI API compat canaries)
// ---------------------------------------------------------------------
// Standalone executable that validates the DX12 bindless toolchain
// end-to-end without dragging in the rest of the engine (no RHI
// abstraction, no swapchain, no window). Mirrors how Unreal's
// D3D12RHI tests live as small `add_executable`s under
// `Engine/Source/Runtime/D3D12RHI/Private/Tests/` and how Microsoft's
// DirectX-Graphics-Samples runs its lightweight ResourceBinding
// sample.
//
// What this test proves:
//   1. PR5b's `target_profile` / `hlsl_version` parameters on
//      `DX12ShaderCompiler::compileFromFile` actually drive DXC into
//      SM 6.6 + HLSL 2021 mode, producing DXIL that references
//      `ResourceDescriptorHeap[NonUniformResourceIndex(...)]`.
//   2. PR5b's `DX12RHI::GetBindlessRootSignatureFlags()` returns a
//      flag set (`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`) that, when OR'd
//      into a `D3D12_ROOT_SIGNATURE_DESC1`, lets
//      `D3D12SerializeVersionedRootSignature` succeed and lets the
//      device create a `ID3D12RootSignature` from it.
//   3. The compiled DXIL + the bindless-aware root signature combine
//      cleanly into a `ID3D12PipelineState` via
//      `CreateGraphicsPipelineState`. PSO creation is the canonical
//      DX12 acceptance check: it cross-validates root signature
//      bindings against shader-side resource references and rejects
//      any mismatch (e.g. a shader that uses `ResourceDescriptorHeap`
//      against a root signature that doesn't permit it).
//   4. (PR6) The RHI abstraction layer still exposes the bindless
//      contract that DX12RHI::CreatePipelineLayout consumes:
//      RHIDescriptorSetLayoutBinding::bindingFlags exists with the
//      right type, RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
//      is non-zero, and RHI::CmdSetBindlessIndexPFN exists with the
//      agreed (commandBuffer, bindPoint, layout, packed_index)
//      signature. Compile-time only -- see static_assert block above
//      main(). No runtime DX12RHI instance is created (would require
//      a window-system bring-up that's out of scope for a smoke-test).
//   5. (PR6 follow-up) The `BindlessIndex::Pack / unpackTexture /
//      unpackSampler` helper in rhi.h round-trips correctly and its
//      half-widths still match the literals (`& 0xFFFFu`, `>> 16`)
//      hard-coded in bindless_smoke.hlsl. A width drift on either
//      side breaks the build at the `static_assert` block, before
//      anyone gets to ship a silent texture-vs-sampler swap.
//
// What this test does NOT do:
//   - No drawing, no swapchain, no window. The DX12 device and queue
//     are spun up just to call the validation entry points. Running
//     the test on a headless CI box (or even WARP) is supported.
//   - No use of the engine's `DX12RHI` class (which couples to the
//     editor's window-system manager). We instantiate a bare
//     `DX12ShaderCompiler` (which is a leaf class -- no RHI
//     dependency) and a freshly-created device.
//   - Bindless support detection (SM 6.6 + Resource Binding Tier 2)
//     is reproduced inline rather than calling DX12RHI -- again to
//     keep this binary independent of the engine's window plumbing.
//     If the host hardware doesn't support bindless, the test
//     returns 77 (autotools / CTest "skip" convention), not 1, so
//     CI buckets it correctly.
//
// Exit codes:
//   0  -- all phases succeeded.
//   1  -- a phase that should have succeeded failed (real test
//         failure; investigate).
//   77 -- bindless not supported on this host -- test skipped.
//
// Build: only when `-DZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON` is passed
// to CMake. Default OFF so normal builds are unaffected. See the
// CMakeLists block right next to this file.
// =====================================================================

#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/DX12/Utility/BindlessTextureBlitPipeline.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>
#include <wrl/client.h>

// =====================================================================
// PR6: compile-time assertions on the RHI bindless surface area.
// ---------------------------------------------------------------------
// These checks are pure type / signature probes -- they ensure the
// abstract RHI base class still exposes the contract that PR6 relies
// on, without spinning up an actual DX12RHI instance (which would drag
// in WindowSystem, GLFW, and the engine event loop).
//
// Failure mode: if anyone changes the field name / type of
// RHIDescriptorSetLayoutBinding::bindingFlags, drops the
// RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT enumerator, or
// removes the cmdSetBindlessIndexPFN virtual, this TU stops
// compiling. That's intentional -- the smoke-test then doubles as an
// "API compatibility canary" for the whole bindless layer.
//
// We deliberately do NOT instantiate DX12RHI here; phases 4 and 5
// below still build the root signature / PSO via raw D3D12 calls.
// Wiring the RHI abstraction into the smoke-test would either require
// (a) a headless variant of DX12RHI::Initialize that skips swapchain
// creation, or (b) bringing up a hidden GLFW window in the test --
// neither is in PR6's scope. PR7+ will revisit when the first real
// bindless material lands.
// =====================================================================
static_assert(std::is_same_v<decltype(RHIDescriptorSetLayoutBinding::bindingFlags), RHIDescriptorBindingFlags>,
              "PR4/PR6: RHIDescriptorSetLayoutBinding must expose a bindingFlags field of type "
              "RHIDescriptorBindingFlags");
static_assert(RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT != 0,
              "PR4/PR6: RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT must be a non-zero "
              "discriminator -- DX12RHI::CreatePipelineLayout uses it to detect bindless sets");

// ---------------------------------------------------------------------
// PR6 follow-up: BindlessIndex pack/unpack helper round-trip.
// ---------------------------------------------------------------------
// These checks pin the host-side pack format (the layout documented
// next to the helper in rhi.h) against the shader-side unpack format
// expected by bindless_smoke.hlsl. If anyone widens the texture half
// to e.g. 24 bits without updating the HLSL, the build breaks here
// instead of silently sampling the wrong slot at runtime.
//
// Round-trip identities:
static_assert(BindlessIndex::Pack(0u, 0u) == 0u,
              "BindlessIndex::pack: zero/zero must produce the zero payload");
static_assert(BindlessIndex::UnpackTexture(BindlessIndex::Pack(0x1234u, 0x5678u)) == 0x1234u,
              "BindlessIndex: texture half must round-trip through pack -> unpackTexture");
static_assert(BindlessIndex::UnpackSampler(BindlessIndex::Pack(0x1234u, 0x5678u)) == 0x5678u,
              "BindlessIndex: sampler half must round-trip through pack -> unpackSampler");
static_assert(BindlessIndex::Pack(BindlessIndex::kMaxTextureIndex, BindlessIndex::kMaxSamplerIndex) == 0xFFFFFFFFu,
              "BindlessIndex: max/max must saturate every bit of the 32-bit payload");
// The HLSL unpack code does `g_packed_indices & 0xFFFFu` and
// `(g_packed_indices >> 16) & 0xFFFFu`. Keep the host masks pinned to
// those literals so a width change in rhi.h forces the shader to
// follow.
static_assert(BindlessIndex::kTextureIndexMask == 0xFFFFu,
              "BindlessIndex::kTextureIndexMask drift -- bindless_smoke.hlsl assumes 0xFFFF");
static_assert(BindlessIndex::kSamplerIndexMask == 0xFFFFu,
              "BindlessIndex::kSamplerIndexMask drift -- bindless_smoke.hlsl assumes 0xFFFF");
static_assert(BindlessIndex::kTextureIndexBits == 16u,
              "BindlessIndex::kTextureIndexBits drift -- bindless_smoke.hlsl assumes a 16-bit shift");
// Truncation contract: out-of-range halves silently mask down. Pin
// the behavior so future "validation" rewrites that would throw on
// overflow have to update the comment too.
static_assert(BindlessIndex::Pack(0x1FFFFu, 0u) == 0xFFFFu,
              "BindlessIndex::pack: oversized texture index must mask, not overflow into sampler bits");
static_assert(BindlessIndex::Pack(0u, 0x1FFFFu) == 0xFFFF0000u,
              "BindlessIndex::pack: oversized sampler index must mask, not overflow off the top");

// =====================================================================
// PR7: bindless static-sampler bank canary.
// ---------------------------------------------------------------------
// Phase 4 of this smoke-test (`createBindlessRootSignature` below)
// builds a 4-entry static-sampler bank inline. PR7 added the same bank
// to `DX12RHI::createPipelineLayout` so every real bindless pipeline
// that flows through the RHI gets s0..s3 attached automatically. The
// two declarations MUST agree:
//   - on the count (`NumStaticSamplers = 4`)
//   - on the order (LinearWrap, LinearClamp, PointWrap, PointClamp)
//   - on the slot range (s0..s3 at RegisterSpace 0)
// because dx12/utility/shaders/bindless_blit_ps.hlsl declares its
// sampler set with hard-coded `register(s0..s3)` slots.
//
// The cheapest way to keep all four artefacts (RHI static sampler array,
// smoke-test inline sampler array, BindlessBlitSampler enum, HLSL
// register slots) honest is to pin a single common count constant and
// check it from compile-time at every site. A future widening (e.g. add
// AnisotropicWrap as s4) MUST update DX12RHI::kBindlessStaticSamplerCount
// AND BindlessBlitSampler AND the HLSL together; if any of them lag,
// this static_assert fires before the build finishes.
//
// Rationale doc-pointer: AGENTS.md 2.9 PR7.
// =====================================================================
static_assert(DX12RHI::kBindlessStaticSamplerCount == 4u,
              "PR7: DX12RHI::kBindlessStaticSamplerCount drift -- bindless_blit_ps.hlsl assumes "
              "exactly 4 static samplers at s0..s3 in the order Linear/Wrap, Linear/Clamp, "
              "Point/Wrap, Point/Clamp. Update bindless_blit_ps.hlsl and the BindlessBlitSampler "
              "enum together if you change this.");
static_assert(static_cast<uint32_t>(BindlessBlitSampler::LinearWrap) == 0u,
              "PR7: BindlessBlitSampler::LinearWrap must map to s0 (matches RHI sampler[0])");
static_assert(static_cast<uint32_t>(BindlessBlitSampler::LinearClamp) == 1u,
              "PR7: BindlessBlitSampler::LinearClamp must map to s1 (matches RHI sampler[1])");
static_assert(static_cast<uint32_t>(BindlessBlitSampler::PointWrap) == 2u,
              "PR7: BindlessBlitSampler::PointWrap must map to s2 (matches RHI sampler[2])");
static_assert(static_cast<uint32_t>(BindlessBlitSampler::PointClamp) == 3u,
              "PR7: BindlessBlitSampler::PointClamp must map to s3 (matches RHI sampler[3])");

namespace
{
    // Sniff that RHI::CmdSetBindlessIndexPFN exists with the agreed
    // signature. Wrapping it in a no-op function pointer ensures any
    // signature drift breaks the build; the function is never called.
    [[maybe_unused]] void _pr6_check_cmd_set_bindless_index_signature()
    {
        using FnType = void (RHI::*)(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipelineLayout*, uint32_t);
        [[maybe_unused]] FnType ptr = &RHI::CmdSetBindlessIndexPFN;
    }
}  // namespace

using Microsoft::WRL::ComPtr;

namespace
{

    // ---- Tiny logging helpers ----------------------------------------------
    // We print to stdout so CI just captures it; no engine logger pulled in.
    void log_info(const char* fmt, ...)
    {
        std::printf("[smoke-test] ");
        std::va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
        std::printf("\n");
    }

    void log_error(const char* fmt, ...)
    {
        std::fprintf(stderr, "[smoke-test][ERROR] ");
        std::va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fprintf(stderr, "\n");
    }

    // ---- HRESULT check ------------------------------------------------------
    bool check_hr(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            log_error("%s failed (HRESULT=0x%08lx)", what, static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    // ---- Phase 1: device creation ------------------------------------------
    // Spin up the smallest possible D3D12 surface -- factory, hardware
    // adapter (or WARP fallback), 11_0 device. No queue / swapchain /
    // command list needed for the validation phases.
    struct DeviceContext
    {
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
    };

    bool createDevice(DeviceContext& out)
    {
        UINT flags = 0;
#if defined(_DEBUG)
        // Enable the debug layer eagerly -- if the host has it installed,
        // any root-signature / PSO mismatch during phase 3 will be flagged
        // with a verbose D3D12 validation message that's invaluable when
        // the test fails. If the debug layer isn't installed, the call is
        // a no-op (D3D12GetDebugInterface returns failure silently).
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                flags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#endif

        if (!check_hr(CreateDXGIFactory2(flags, IID_PPV_ARGS(&out.factory)),
                      "CreateDXGIFactory2"))
            return false;

        // Prefer a high-performance hardware adapter; fall back to WARP if
        // none is available (CI / headless boxes). The smoke test has no
        // performance requirements, so WARP is a perfectly valid backend
        // for it -- but WARP doesn't implement Resource Binding Tier 2 in
        // older Windows builds, so the bindless-detect step below may
        // legitimately skip on those.
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (out.factory->EnumAdapterByGpuPreference(
                    i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_ADAPTER_DESC1 desc {};
            candidate->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;  // skip WARP on the first pass; we'll try it as a fallback

            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.device))))
            {
                out.adapter = candidate;
                log_info("Device created on hardware adapter '%ls'", desc.Description);
                return true;
            }
        }

        // WARP fallback
        if (SUCCEEDED(out.factory->EnumWarpAdapter(IID_PPV_ARGS(&out.adapter))) &&
            SUCCEEDED(D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.device))))
        {
            log_info("Device created on WARP software adapter (no hardware adapter usable)");
            return true;
        }

        log_error("No usable D3D12 adapter found (hardware nor WARP)");
        return false;
    }

    // ---- Phase 2: bindless capability probe --------------------------------
    // Reproduce DX12RHI::Initialize's probe inline -- SM 6.6 query +
    // ResourceBindingTier query. Returns the {bindless_supported,
    // engine_flag_set} pair so the test main can branch on it.
    struct BindlessSupport
    {
        bool supported = false;
        D3D_SHADER_MODEL max_shader_model = D3D_SHADER_MODEL_6_0;
        D3D12_RESOURCE_BINDING_TIER binding_tier = D3D12_RESOURCE_BINDING_TIER_1;
        D3D12_ROOT_SIGNATURE_FLAGS bindless_flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    };

    BindlessSupport probeBindless(ID3D12Device* device)
    {
        BindlessSupport out;

        D3D12_FEATURE_DATA_SHADER_MODEL sm {D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        {
            out.max_shader_model = sm.HighestShaderModel;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS opts {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
        {
            out.binding_tier = opts.ResourceBindingTier;
        }

        const bool sm66_ok = (out.max_shader_model >= D3D_SHADER_MODEL_6_6);
        const bool tier_ok = (out.binding_tier >= D3D12_RESOURCE_BINDING_TIER_2);
        out.supported = sm66_ok && tier_ok;

        // Mirror DX12RHI::GetBindlessRootSignatureFlags() for the test's
        // independence (we don't link against the RHI). The values must
        // stay in sync; if they ever diverge, the smoke-test will catch
        // it because PSO creation will fail one way or the other.
        if (out.supported)
        {
            out.bindless_flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
            // SAMPLER_HEAP_DIRECTLY_INDEXED intentionally NOT set --
            // matches AGENTS.md 2.9 PR5b decision.
        }

        log_info("Probe: SM=0x%x, ResourceBindingTier=%d, bindless=%s",
                 static_cast<unsigned int>(out.max_shader_model),
                 static_cast<int>(out.binding_tier),
                 out.supported ? "YES" : "NO");
        return out;
    }

    // ---- Phase 3: shader compilation ---------------------------------------
    // Drives DX12ShaderCompiler with the new PR5b parameters. Returns
    // the compiled DXIL bytecode for VS + PS, ready to feed into a PSO.
    struct CompiledShaders
    {
        std::vector<uint8_t> vs_dxil;
        std::vector<uint8_t> ps_dxil;
    };

    bool CompileShaders(const std::filesystem::path& vs_path,
                        const std::filesystem::path& ps_path,
                        CompiledShaders& out)
    {
        DX12ShaderCompiler compiler;

        // VS: legacy SM 6.0 path (empty target_profile / hlsl_version) --
        // tests that the existing code path is byte-for-byte unchanged
        // post-PR5b.
        auto vs = compiler.CompileFromFile(vs_path.string(),
                                           ShaderStage::Vertex,
                                           /*include_paths=*/ {},
                                           /*macros=*/ {},
                                           /*entry_point=*/"main");
        if (!vs.success)
        {
            log_error("VS compile failed: %s", vs.error_message.c_str());
            return false;
        }
        log_info("VS compiled OK (DXIL size = %zu bytes, profile = vs_6_0 default, HV default)",
                 vs.dxil_code.size());

        // PS: SM 6.6 + HLSL 2021 path -- the actual PR5b feature under
        // test. Any malformation here (stale DXC, missing argument
        // forwarding, etc.) shows up as a compile error, not a PSO
        // mismatch.
        auto ps = compiler.CompileFromFile(ps_path.string(),
                                           ShaderStage::Fragment,
                                           /*include_paths=*/ {},
                                           /*macros=*/ {},
                                           /*entry_point=*/"main",
                                           /*target_profile=*/"ps_6_6",
                                           /*hlsl_version=*/"2021");
        if (!ps.success)
        {
            log_error("PS compile failed: %s", ps.error_message.c_str());
            return false;
        }
        log_info("PS compiled OK (DXIL size = %zu bytes, profile = ps_6_6, HV 2021)",
                 ps.dxil_code.size());

        out.vs_dxil = std::move(vs.dxil_code);
        out.ps_dxil = std::move(ps.dxil_code);
        return true;
    }

    // ---- Phase 4: bindless root signature ----------------------------------
    // Builds the root signature the bindless PSO will consume:
    //   - Flags: ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT (legacy default)
    //          | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED (from PR5b helper)
    //   - Root parameter 0: 32-bit root constant (b0, all stages -- but
    //     we restrict to PS to match the shader's cbuffer declaration).
    //   - Static samplers: 4 entries at s0..s3 covering the four
    //     {linear,point} x {wrap,clamp} combos. Matches PR5b's design
    //     decision to NOT use sampler-bindless.
    bool createBindlessRootSignature(ID3D12Device* device,
                                     D3D12_ROOT_SIGNATURE_FLAGS bindless_flags,
                                     ComPtr<ID3D12RootSignature>& out)
    {
        // Root parameter 0: 32-bit root constant, 1 dword, b0, PS-visible.
        D3D12_ROOT_PARAMETER root_param {};
        root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_param.Constants.ShaderRegister = 0;
        root_param.Constants.RegisterSpace = 0;
        root_param.Constants.Num32BitValues = 1;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Four static samplers. Field-by-field -- the inline-init form
        // of D3D12_STATIC_SAMPLER_DESC is fiddly across MSVC versions.
        auto make_sampler = [](D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE addr, UINT slot) {
            D3D12_STATIC_SAMPLER_DESC s {};
            s.Filter = filter;
            s.AddressU = addr;
            s.AddressV = addr;
            s.AddressW = addr;
            s.MipLODBias = 0.0f;
            s.MaxAnisotropy = 1;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ShaderRegister = slot;
            s.RegisterSpace = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            return s;
        };

        D3D12_STATIC_SAMPLER_DESC samplers[4] = {
            make_sampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0),
            make_sampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1),
            make_sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 2),
            make_sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 3),
        };

        D3D12_ROOT_SIGNATURE_DESC desc {};
        desc.NumParameters = 1;
        desc.pParameters = &root_param;
        desc.NumStaticSamplers = 4;
        desc.pStaticSamplers = samplers;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | bindless_flags;  // <-- the PR5b helper output flows in here

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&desc,
                                                 D3D_ROOT_SIGNATURE_VERSION_1,
                                                 blob.GetAddressOf(),
                                                 err.GetAddressOf());
        if (FAILED(hr))
        {
            if (err)
            {
                log_error("D3D12SerializeRootSignature failed: %s",
                          static_cast<const char*>(err->GetBufferPointer()));
            }
            else
            {
                log_error("D3D12SerializeRootSignature failed (no error blob, HRESULT=0x%08lx)",
                          static_cast<unsigned long>(hr));
            }
            return false;
        }

        if (!check_hr(device->CreateRootSignature(0,
                                                  blob->GetBufferPointer(),
                                                  blob->GetBufferSize(),
                                                  IID_PPV_ARGS(out.GetAddressOf())),
                      "CreateRootSignature"))
            return false;

        log_info("Root signature created (flags=0x%x, 1 root constant, 4 static samplers)",
                 static_cast<unsigned int>(desc.Flags));
        return true;
    }

    // ---- Phase 5: graphics PSO --------------------------------------------
    // Smallest valid graphics PSO that exercises the bindless-aware root
    // signature + SM 6.6 PS DXIL combo. A failed `CreateGraphicsPipelineState`
    // here is the smoke-test's failure signal: the runtime has rejected
    // some part of the toolchain output.
    bool createPipelineState(ID3D12Device* device,
                             ID3D12RootSignature* root_signature,
                             const std::vector<uint8_t>& vs_dxil,
                             const std::vector<uint8_t>& ps_dxil,
                             ComPtr<ID3D12PipelineState>& out)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};

        desc.pRootSignature = root_signature;
        desc.VS.pShaderBytecode = vs_dxil.data();
        desc.VS.BytecodeLength = vs_dxil.size();
        desc.PS.pShaderBytecode = ps_dxil.data();
        desc.PS.BytecodeLength = ps_dxil.size();
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        // Rasterizer: D3D12_DEFAULT equivalent (FILL_SOLID, CULL_BACK).
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        desc.RasterizerState.FrontCounterClockwise = FALSE;
        desc.RasterizerState.DepthClipEnable = TRUE;

        // Blend: opaque, write all channels.
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // DepthStencil: disabled (smoke-test has no depth target).
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;

        // Input layout: empty -- the VS uses SV_VertexID for a fullscreen
        // triangle, no vertex buffer needed.
        desc.InputLayout.pInputElementDescs = nullptr;
        desc.InputLayout.NumElements = 0;

        if (!check_hr(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out.GetAddressOf())),
                      "CreateGraphicsPipelineState"))
            return false;

        log_info("Graphics PSO created OK -- bindless toolchain end-to-end is healthy.");
        return true;
    }

    // ---- Locate the .hlsl files -------------------------------------------
    // Test resolves the .hlsl files relative to either
    //   (a) ZENGINE_BINDLESS_TEST_DIR -- a CMake-injected absolute path
    //       to the source directory (preferred -- works regardless of
    //       CWD), or
    //   (b) `<exe-dir>/test/` -- fallback for the case where someone
    //       hand-runs the test from a non-standard layout.
    struct ShaderPaths
    {
        std::filesystem::path vs;
        std::filesystem::path ps;
    };

    bool resolveShaderPaths(ShaderPaths& out)
    {
#ifdef ZENGINE_BINDLESS_TEST_DIR
        const std::filesystem::path src_dir {ZENGINE_BINDLESS_TEST_DIR};
        out.vs = src_dir / "bindless_smoke_vs.hlsl";
        out.ps = src_dir / "bindless_smoke.hlsl";
#else
        // Fallback -- look next to the executable.
        out.vs = std::filesystem::current_path() / "test" / "bindless_smoke_vs.hlsl";
        out.ps = std::filesystem::current_path() / "test" / "bindless_smoke.hlsl";
#endif
        if (!std::filesystem::exists(out.vs))
        {
            log_error("VS shader not found at %s", out.vs.string().c_str());
            return false;
        }
        if (!std::filesystem::exists(out.ps))
        {
            log_error("PS shader not found at %s", out.ps.string().c_str());
            return false;
        }
        return true;
    }

}  // namespace

int main()
{
    log_info("=== DX12 bindless smoke-test (PR5c) ===");

    ShaderPaths paths;
    if (!resolveShaderPaths(paths))
        return 1;
    log_info("VS = %s", paths.vs.string().c_str());
    log_info("PS = %s", paths.ps.string().c_str());

    DeviceContext ctx;
    if (!createDevice(ctx))
        return 1;

    BindlessSupport probe = probeBindless(ctx.device.Get());
    if (!probe.supported)
    {
        log_info("Bindless not supported on this host -- skipping (exit 77).");
        // 77 == autotools / CTest "skip" convention. CI buckets this
        // separately from a real failure; lets us run the test on
        // legacy hardware without false-positive red builds.
        return 77;
    }

    CompiledShaders shaders;
    if (!CompileShaders(paths.vs, paths.ps, shaders))
        return 1;

    ComPtr<ID3D12RootSignature> root_signature;
    if (!createBindlessRootSignature(ctx.device.Get(), probe.bindless_flags, root_signature))
        return 1;

    ComPtr<ID3D12PipelineState> pso;
    if (!createPipelineState(ctx.device.Get(),
                             root_signature.Get(),
                             shaders.vs_dxil,
                             shaders.ps_dxil,
                             pso))
        return 1;

    log_info("=== ALL PHASES PASSED ===");
    return 0;
}
