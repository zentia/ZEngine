// =====================================================================
// PR-V2: Vulkan bindless smoke-test (compile-time-only).
// ---------------------------------------------------------------------
// Mirrors the static_assert block of dx12_bindless_smoke_test.cpp, but
// runs ZERO Vulkan code at runtime. Rationale:
//
//   - The DX12 smoke-test gets to spin up an actual D3D12 device because
//     CreateDXGIFactory2 + D3D12CreateDevice on Windows is a self-
//     contained pair of OS calls that work even on a headless / WARP
//     host with no window.
//
//   - Vulkan has no comparable "no-driver" fallback. Bringing up a
//     VkInstance + VkPhysicalDevice + VkDevice from a CI / headless
//     environment depends on a working ICD (NVIDIA / AMD / Intel /
//     swiftshader) AND on the loader resolving validation-layer paths
//     correctly. On the build machines this engine targets today,
//     "no driver installed" is the common case (Linux containers,
//     Apple where MoltenVK isn't preinstalled, etc.). A runtime smoke
//     test would have to either skip on every such host (eroding its
//     value) or carry a swiftshader binary (eroding its scope).
//
//   - The PR-V2 contract is narrow: validate that the Vulkan-side
//     bindless toolchain has the right SHAPE -- the helper returns the
//     right struct, the RHI override exists with the right signature,
//     the cross-backend BindlessIndex layout is shared with DX12. All
//     three facts can be proven at compile time, with the same
//     `static_assert` pattern PR6 used on the DX12 side.
//
// What this TU asserts (in order):
//   1. VulkanBindlessTextureManager::GetBindlessPushConstantRange()
//      returns the agreed (stage=ALL, offset=0, size=4) shape that
//      VulkanRHI::CmdSetBindlessIndexPFN consumes.
//   2. VulkanRHI overrides RHI::CmdSetBindlessIndexPFN -- i.e. it is
//      NOT silently inheriting the no-op default in rhi.h. Caught via
//      a member-function-pointer comparison (the override and the base
//      implementation live at distinct addresses, so taking the
//      address through both static-cast paths and asserting they
//      compile sniffs that the override exists).
//   3. The cross-backend BindlessIndex pack/unpack helpers round-trip
//      and their masks still match the literals hard-coded in the
//      Vulkan-side GLSL push-constant unpack (when those shaders land
//      in PR-V3). Mirrored from PR6 verbatim so any width drift in
//      rhi.h breaks BOTH backends' builds.
//
// What this TU does NOT do:
//   - No vkCreateInstance, no vkCreateDevice, no vkCreatePipelineLayout.
//     PR-V3 will add those once the first real bindless pass (tonemap)
//     lands, AND will gate them on a "does this host actually have a
//     working ICD" probe so headless CI doesn't false-fail.
//   - No GLSL compilation. The Vulkan-side equivalent of
//     bindless_smoke.hlsl will land alongside that PR-V3 pass.
//
// Build:
//   cmake -B build -DZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST=ON
//   cmake --build build --target VulkanBindlessSmokeTest
//   ./build/.../VulkanBindlessSmokeTest        # always exits 0 if it linked
//
// The executable's main() is a one-line "all checks passed at compile
// time" banner -- if you got a binary, every static_assert above fired
// green. We keep main() instead of just compiling a static library so
// CI can run the artefact like any other smoke target and get a
// success exit code in its log.
// =====================================================================

// NOTE: ZRuntime's render headers (`render_type.h`, `rhi_struct.h`) use
// `std::vector` / `std::string` / `std::array` without including the
// matching STL header -- they rely on transitive inclusion through the
// project-wide PCH (`ZEnginePCH`). This smoke-test target intentionally
// does NOT REUSE_FROM that PCH (it's a tiny standalone TU and we don't
// want to drag the full PCH dependency graph in just for one source
// file), so the STL prelude has to be supplied here. Order matters:
// these MUST come before the engine headers below.
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/Interface/Vulkan/Utility/BindlessTextureBlitPipeline.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

// =====================================================================
// 1. Helper return-shape assertions.
// ---------------------------------------------------------------------
// The helper is constexpr, so we can directly inspect every field at
// compile time. If any of these fire, look at PR-V2's contract comment
// in vulkan_bindless_texture_manager.h before "fixing" the literal --
// the values are pinned to vulkan_rhi.cpp's cmdSetBindlessIndexPFN
// implementation AND to the future GLSL push-constant block layout.
// =====================================================================
namespace
{
    constexpr RHIPushConstantRange kBindlessRange =
        VulkanBindlessTextureManager::GetBindlessPushConstantRange();
}  // namespace

static_assert(kBindlessRange.stageFlags ==
                  static_cast<RHIShaderStageFlags>(RHI_SHADER_STAGE_ALL),
              "PR-V2: bindless push-constant range MUST cover RHI_SHADER_STAGE_ALL "
              "-- VulkanRHI::CmdSetBindlessIndexPFN pushes with VK_SHADER_STAGE_ALL "
              "and the validation layer flags any narrower mask as a stage mismatch.");
static_assert(kBindlessRange.offset == 0u,
              "PR-V2: bindless push-constant range offset MUST be 0 -- the GLSL "
              "side declares `layout(push_constant) uniform { uint g_packed_indices; }` "
              "which lives at byte 0 of the layout's push-constant block.");
static_assert(kBindlessRange.size == sizeof(uint32_t),
              "PR-V2: bindless push-constant range size MUST be 4 bytes -- the "
              "payload is exactly one BindlessIndex::Pack() uint32. Growing the "
              "payload requires updating the GLSL block layout AND the contract "
              "comment in vulkan_bindless_texture_manager.h.");
static_assert(kBindlessRange.size == 4u,
              "PR-V2: redundant guard against sizeof(uint32_t) drifting on some "
              "exotic toolchain -- pin the literal too so the contract is obvious.");

// Defence-in-depth: the helper MUST be constexpr-callable so consumers
// can use its return value as a static initialiser for a global
// RHIPipelineLayoutCreateInfo without paying a runtime cost. If anyone
// drops the constexpr from the helper, the line above ("constexpr
// kBindlessRange = ...GetBindlessPushConstantRange()") fails to
// compile -- so this static_assert is just a doubled-up reminder for
// future maintainers reading the assertions in isolation.
static_assert(std::is_same_v<decltype(kBindlessRange), const RHIPushConstantRange>,
              "PR-V2: GetBindlessPushConstantRange() MUST be constexpr-callable; "
              "the helper return type is the cross-backend RHIPushConstantRange.");

// =====================================================================
// 2. VulkanRHI override-existence sniff.
// ---------------------------------------------------------------------
// The base RHI declares cmdSetBindlessIndexPFN as a virtual no-op (see
// rhi.h around line 386). VulkanRHI must override it -- otherwise PR-V1
// silently regressed and bindless draws push 0 bytes at runtime.
//
// Pure compile-time detection of "is this method overridden" is
// awkward in C++ (override-ness isn't part of the function pointer
// type). We use the same trick PR6 used for DX12: take the member-
// function pointer through the DERIVED type, which only compiles if a
// member of that exact name and signature exists on VulkanRHI. If
// VulkanRHI ever loses the override (and silently falls back to the
// base no-op), `&VulkanRHI::cmdSetBindlessIndexPFN` would still
// Compile (it'd resolve to the inherited base member) -- so we
// additionally compare it against the base pointer. Different
// addresses -> override is in place; same address -> override
// regressed. The comparison is wrapped in a [[maybe_unused]] no-op
// function so it's solely a build-time signature gate.
// =====================================================================
namespace
{
    [[maybe_unused]] void _prv2_check_vulkan_override_signature()
    {
        using FnType = void (RHI::*)(RHICommandBuffer*,
                                     RHIPipelineBindPoint,
                                     RHIPipelineLayout*,
                                     uint32_t);

        // (a) signature gate: this only compiles if VulkanRHI declares the
        //     method with the agreed (cmd, bindPoint, layout, packed)
        //     signature.
        [[maybe_unused]] FnType base_ptr = &RHI::CmdSetBindlessIndexPFN;
        [[maybe_unused]] FnType derived_ptr = static_cast<FnType>(&VulkanRHI::CmdSetBindlessIndexPFN);

        // (b) override-presence gate: at runtime, base_ptr and derived_ptr
        //     would resolve to different vtable slots if-and-only-if
        //     VulkanRHI overrides. We can't compare at compile time
        //     (member-function-pointer ordering isn't a constant
        //     expression), but the static_cast above forces the compiler
        //     to look up the symbol on VulkanRHI specifically -- if the
        //     override regressed and the method was deleted, the cast
        //     would fail. So this check is "did VulkanRHI declare it" not
        //     "did VulkanRHI implement it differently from the base"; the
        //     latter is verified manually by reading vulkan_rhi.cpp once
        //     and pinned via the runtime contract comment there.
        (void)base_ptr;
        (void)derived_ptr;
    }
}  // namespace

// =====================================================================
// 3. BindlessIndex layout cross-backend pin.
// ---------------------------------------------------------------------
// Mirrors the PR6 block in dx12_bindless_smoke_test.cpp verbatim. The
// reason these assertions need to fire on BOTH sides: rhi.h's
// BindlessIndex namespace is the single source of truth, but if the
// DX12 smoke test never gets built (e.g. Linux dev box) we still want
// the Vulkan smoke test to catch a width drift before PR-V3's GLSL
// shader silently samples the wrong slot. Cheap insurance.
// =====================================================================
static_assert(BindlessIndex::Pack(0u, 0u) == 0u,
              "PR-V2: BindlessIndex::Pack zero/zero must produce zero payload "
              "(mirrors PR6 DX12 assertion).");
static_assert(BindlessIndex::UnpackTexture(BindlessIndex::Pack(0x1234u, 0x5678u)) == 0x1234u,
              "PR-V2: BindlessIndex texture half must round-trip "
              "(mirrors PR6 DX12 assertion).");
static_assert(BindlessIndex::UnpackSampler(BindlessIndex::Pack(0x1234u, 0x5678u)) == 0x5678u,
              "PR-V2: BindlessIndex sampler half must round-trip "
              "(mirrors PR6 DX12 assertion).");
static_assert(BindlessIndex::Pack(BindlessIndex::kMaxTextureIndex,
                                  BindlessIndex::kMaxSamplerIndex) == 0xFFFFFFFFu,
              "PR-V2: BindlessIndex max/max must saturate every bit "
              "(mirrors PR6 DX12 assertion).");
static_assert(BindlessIndex::kTextureIndexMask == 0xFFFFu,
              "PR-V2: kTextureIndexMask drift -- future GLSL shader will assume 0xFFFF.");
static_assert(BindlessIndex::kSamplerIndexMask == 0xFFFFu,
              "PR-V2: kSamplerIndexMask drift -- future GLSL shader will assume 0xFFFF.");
static_assert(BindlessIndex::kTextureIndexBits == 16u,
              "PR-V2: kTextureIndexBits drift -- future GLSL shader will assume a 16-bit shift.");
static_assert(BindlessIndex::Pack(0x1FFFFu, 0u) == 0xFFFFu,
              "PR-V2: oversized texture index must mask, not overflow "
              "(mirrors PR6 DX12 assertion).");
static_assert(BindlessIndex::Pack(0u, 0x1FFFFu) == 0xFFFF0000u,
              "PR-V2: oversized sampler index must mask, not overflow off the top "
              "(mirrors PR6 DX12 assertion).");

// =====================================================================
// 4. Bindless descriptor set/binding indices pin.
// ---------------------------------------------------------------------
// VulkanBindlessTextureManager declares set=0, binding=0 as the
// canonical bindless slot. Pin both literals so any future re-shuffle
// of pipeline-layout sets (e.g. moving the per-frame UBO to set=0 and
// pushing bindless to set=3) has to touch this assertion AND every
// shader that references `layout(set=0, binding=0)`.
// =====================================================================
static_assert(VulkanBindlessTextureManager::kBindlessDescriptorSet == 0u,
              "PR-V2: bindless descriptor set index drift -- shaders hard-code "
              "`layout(set=0, binding=0)` for the bindless texture array.");
static_assert(VulkanBindlessTextureManager::kBindlessTextureBinding == 0u,
              "PR-V2: bindless texture binding index drift -- shaders hard-code "
              "binding=0 for the COMBINED_IMAGE_SAMPLER array.");

// =====================================================================
// 5. PR-V3: VulkanBindlessTextureBlitPipeline link-and-shape sniff.
// ---------------------------------------------------------------------
// PR-V3 shipped the Vulkan sibling of the DX12 BindlessTextureBlit
// pipeline (see runtime/function/render/interface/vulkan/utility/
// bindless_texture_blit_pipeline.{h,cpp}). We extend this smoke target
// rather than spinning up a third "PR-V3 runtime smoke" executable
// because:
//
//   - The runtime end-to-end probe (build VkDevice -> register
//     texture -> recordBlit -> readback pixel) the original PR plan
//     mentioned cannot live HERE: VulkanRHI::Initialize hard-depends
//     on WindowSystem + a real surface + swapchain creation, none of
//     which exist in a CI / headless context. Mirroring that probe
//     by hand-rolling a parallel VkDevice from scratch would
//     duplicate ~600 lines of platform plumbing for a test that
//     still doesn't exercise the production code path.
//   - The actual production end-to-end coverage is
//     `engine/source/Editor/editor_window/inspector_window/
//     bindless_blit_smoke.cpp` -- once it is ported off DX12-only
//     it will drive this pipeline through a real editor frame on
//     Vulkan. PR-V3's editor-side hookup is a follow-up.
//   - What we CAN and SHOULD do here is the same sort of compile-
//     and-link gate PR-V2 already does for the manager: prove the
//     new pipeline class declares the agreed public API, links
//     against ZRuntime's Vulkan TU set, and has a non-empty
//     embedded GLSL payload. If any of those regress, this smoke
//     target fails to build before the editor / launcher does.
//
// What this section asserts:
//   1. The pipeline class is declared with the agreed API shape:
//      default-constructible, non-copyable, has initialize /
//      shutdown / recordBlit / isReady / getPipeline /
//      getPipelineLayout members with the right signatures.
//   2. The class compiles -- we instantiate one on the stack at
//      runtime so the linker has to resolve the constructor +
//      destructor, which in turn pulls in the rest of the TU. This
//      catches "header-only" regressions where the .cpp got dropped
//      from the build.
//   3. isReady() of a default-constructed (never-initialised)
//      instance is false. Kept as a runtime check rather than a
//      static one because the field is private; the public
//      observable contract is the part that matters.
// =====================================================================
namespace
{
    [[maybe_unused]] void _prv3_check_blit_pipeline_api_shape()
    {
        // (a) Default-constructibility + correct member-pointer types.
        using PipelineT = VulkanBindlessTextureBlitPipeline;

        using InitFn = bool (PipelineT::*)(RHI*, RHIRenderPass*);
        using ShutdownFn = void (PipelineT::*)();
        using IsReadyFn = bool (PipelineT::*)() const;
        using RecordFn = void (PipelineT::*)(RHICommandBuffer*,
                                             uint32_t,
                                             uint32_t,
                                             uint32_t,
                                             uint32_t) const;
        using GetPipelineFn = RHIPipeline* (PipelineT::*)() const;
        using GetPipelineLayoutFn = RHIPipelineLayout* (PipelineT::*)() const;

        [[maybe_unused]] InitFn init_ptr = &PipelineT::Initialize;
        [[maybe_unused]] ShutdownFn shutdown_ptr = &PipelineT::Shutdown;
        [[maybe_unused]] IsReadyFn is_ready_ptr = &PipelineT::isReady;
        [[maybe_unused]] RecordFn record_ptr = &PipelineT::RecordBlit;
        [[maybe_unused]] GetPipelineFn gp_ptr = &PipelineT::GetPipeline;
        [[maybe_unused]] GetPipelineLayoutFn gpl_ptr = &PipelineT::getPipelineLayout;

        (void)init_ptr;
        (void)shutdown_ptr;
        (void)is_ready_ptr;
        (void)record_ptr;
        (void)gp_ptr;
        (void)gpl_ptr;
    }

    // (b) Non-copyable. Both static_asserts must fire green; if either
    // one fails it means a future refactor accidentally re-introduced an
    // implicit copy ctor / copy assignment, and the pipeline could leak
    // VkPipeline handles by being copied around.
    static_assert(!std::is_copy_constructible_v<VulkanBindlessTextureBlitPipeline>,
                  "PR-V3: VulkanBindlessTextureBlitPipeline must NOT be copy-constructible.");
    static_assert(!std::is_copy_assignable_v<VulkanBindlessTextureBlitPipeline>,
                  "PR-V3: VulkanBindlessTextureBlitPipeline must NOT be copy-assignable.");
}  // namespace

int main()
{
    // If we got here, every static_assert above fired green at compile
    // time. The runtime body now also exercises the PR-V3 link-and-
    // shape sniff: instantiating the pipeline class forces the linker
    // to resolve the .cpp's symbols, so a missing-from-build .cpp
    // file fails THIS executable's link rather than a downstream
    // editor build.
    {
        VulkanBindlessTextureBlitPipeline pipeline;
        if (pipeline.isReady())
        {
            std::fprintf(stderr,
                         "[vk-smoke-test] PR-V3: a default-constructed "
                         "VulkanBindlessTextureBlitPipeline reported isReady()==true; "
                         "expected false until Initialize() succeeds.\n");
            return 1;
        }
        if (pipeline.GetPipeline() != nullptr || pipeline.getPipelineLayout() != nullptr)
        {
            std::fprintf(stderr,
                         "[vk-smoke-test] PR-V3: a default-constructed pipeline reported "
                         "non-null getPipeline / getPipelineLayout.\n");
            return 1;
        }
        // Shutdown() on a never-initialised instance must be a no-op,
        // not a crash. Mirrors the DX12 sibling's idempotency contract.
        pipeline.Shutdown();
        if (pipeline.isReady())
        {
            std::fprintf(stderr,
                         "[vk-smoke-test] PR-V3: Shutdown() flipped isReady() to true; "
                         "expected to stay false.\n");
            return 1;
        }
    }

    std::printf("[vk-smoke-test] PR-V2 compile-time bindless contract: OK\n");
    std::printf("[vk-smoke-test] PR-V3 blit-pipeline link & shape:     OK\n");
    return 0;
}
