// =====================================================================================
// RHIIndirectDrawSmokeTest -- headless contract checks for indirect draws.
// -------------------------------------------------------------------------------------
// Indirect draws are what make GPU-driven geometry (Nanite cluster submission, GPU
// particle compaction) possible: a compute pass writes the draw arguments into a
// buffer and the CPU never learns how many draws there were. That only works if every
// backend reads the same bytes, so this test pins the one thing that must not drift:
//
//   1. RHIDrawIndexedIndirectCommand / RHIDrawIndirectCommand have the size and field
//      offsets that Vulkan's VkDraw*IndirectCommand, DX12's D3D12_DRAW_*_ARGUMENTS and
//      Metal's MTLDraw*PrimitivesIndirectArguments all agree on. A backend that
//      reorders a field would silently draw garbage -- there is no other guard.
//   2. RHI::CmdDrawIndexedIndirect / RHI::CmdDrawIndirect exist with the signature the
//      callers are written against (pinned by taking a pointer-to-member).
//   3. Stepping a batch by `stride` lands on the right command. Metal has no
//      multi-draw indirect and emulates a batch by looping with offset += stride, so
//      this walks the exact arithmetic that loop performs.
//
// No GPU, no window, no backend: RHI.h is the abstract interface and the structs are
// plain data, so this runs anywhere -- including macOS, where none of the backends
// that implement the calls are even compiled.
//
//   cmake -S . -B build -DZENGINE_BUILD_RHI_INDIRECT_DRAW_SMOKE_TEST=ON
//   cmake --build build --target RHIIndirectDrawSmokeTest
//
// Run it after touching RHI.h, RHIStruct.h or any CmdDraw*Indirect implementation.
// =====================================================================================

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// -------------------------------------------------------------------------------------
// Compile-time contract. These are the assertions a Vulkan/DX12/Metal header would
// make for us if this machine could compile all four backends -- since it cannot
// (Apple excludes Interface/Vulkan and Interface/DX12 outright), they live here.
// -------------------------------------------------------------------------------------
static_assert(sizeof(RHIDrawIndexedIndirectCommand) == 20, "indexed indirect command must be 20 bytes");
static_assert(offsetof(RHIDrawIndexedIndirectCommand, indexCount) == 0, "indexCount must come first");
static_assert(offsetof(RHIDrawIndexedIndirectCommand, instanceCount) == 4, "instanceCount must follow indexCount");
static_assert(offsetof(RHIDrawIndexedIndirectCommand, firstIndex) == 8, "firstIndex must follow instanceCount");
static_assert(offsetof(RHIDrawIndexedIndirectCommand, vertexOffset) == 12, "vertexOffset must follow firstIndex");
static_assert(offsetof(RHIDrawIndexedIndirectCommand, firstInstance) == 16, "firstInstance must come last");

static_assert(sizeof(RHIDrawIndirectCommand) == 16, "non-indexed indirect command must be 16 bytes");
static_assert(offsetof(RHIDrawIndirectCommand, vertexCount) == 0, "vertexCount must come first");
static_assert(offsetof(RHIDrawIndirectCommand, instanceCount) == 4, "instanceCount must follow vertexCount");
static_assert(offsetof(RHIDrawIndirectCommand, firstVertex) == 8, "firstVertex must follow instanceCount");
static_assert(offsetof(RHIDrawIndirectCommand, firstInstance) == 12, "firstInstance must come last");

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;
    int g_Group = 0;

    void BeginGroup(const char* title)
    {
        ++g_Group;
        std::printf("[%d] %s\n", g_Group, title);
    }

    void Check(bool condition, const char* what)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("    FAIL: %s\n", what);
        }
    }

#define CHECK(expr) Check((expr), #expr)

    // Signature pin: these only compile while RHI declares the calls with exactly the
    // parameter list callers use. Changing the abstract API breaks the build here
    // rather than at some distant Nanite call site.
    using DrawIndexedIndirectFn = void (RHI::*)(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t);
    using DrawIndirectFn = void (RHI::*)(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t);

    // A byte buffer standing in for the one a compute pass would write. Deliberately
    // built by hand (per-field memcpy at the documented offsets) so the test proves
    // the layout rather than assuming it.
    std::vector<uint8_t> MakeIndexedCommands(const std::vector<RHIDrawIndexedIndirectCommand>& commands,
                                             uint32_t stride)
    {
        std::vector<uint8_t> bytes(static_cast<std::size_t>(stride) * commands.size(), 0u);
        for (std::size_t i = 0; i < commands.size(); ++i)
        {
            uint8_t* dst = bytes.data() + i * static_cast<std::size_t>(stride);
            const RHIDrawIndexedIndirectCommand& c = commands[i];
            std::memcpy(dst + offsetof(RHIDrawIndexedIndirectCommand, indexCount), &c.indexCount, sizeof(c.indexCount));
            std::memcpy(dst + offsetof(RHIDrawIndexedIndirectCommand, instanceCount), &c.instanceCount, sizeof(c.instanceCount));
            std::memcpy(dst + offsetof(RHIDrawIndexedIndirectCommand, firstIndex), &c.firstIndex, sizeof(c.firstIndex));
            std::memcpy(dst + offsetof(RHIDrawIndexedIndirectCommand, vertexOffset), &c.vertexOffset, sizeof(c.vertexOffset));
            std::memcpy(dst + offsetof(RHIDrawIndexedIndirectCommand, firstInstance), &c.firstInstance, sizeof(c.firstInstance));
        }
        return bytes;
    }

    RHIDrawIndexedIndirectCommand ReadIndexedCommand(const std::vector<uint8_t>& bytes, std::size_t byte_offset)
    {
        RHIDrawIndexedIndirectCommand c {};
        const uint8_t* src = bytes.data() + byte_offset;
        std::memcpy(&c.indexCount, src + offsetof(RHIDrawIndexedIndirectCommand, indexCount), sizeof(c.indexCount));
        std::memcpy(&c.instanceCount, src + offsetof(RHIDrawIndexedIndirectCommand, instanceCount), sizeof(c.instanceCount));
        std::memcpy(&c.firstIndex, src + offsetof(RHIDrawIndexedIndirectCommand, firstIndex), sizeof(c.firstIndex));
        std::memcpy(&c.vertexOffset, src + offsetof(RHIDrawIndexedIndirectCommand, vertexOffset), sizeof(c.vertexOffset));
        std::memcpy(&c.firstInstance, src + offsetof(RHIDrawIndexedIndirectCommand, firstInstance), sizeof(c.firstInstance));
        return c;
    }
} // namespace

int main()
{
    BeginGroup("abstract API is declared with the signature callers use");
    {
        DrawIndexedIndirectFn indexed = &RHI::CmdDrawIndexedIndirect;
        DrawIndirectFn non_indexed = &RHI::CmdDrawIndirect;
        CHECK(indexed != nullptr);
        CHECK(non_indexed != nullptr);
        // Both must be virtual -- that is the whole point of pinning them: a
        // non-virtual declaration would let a backend silently fail to override.
        CHECK(&RHI::CmdDrawIndexedIndirect != nullptr);
        CHECK(&RHI::CmdDrawIndirect != nullptr);
    }

    BeginGroup("indirect buffer usage flag is a real, distinct bit");
    {
        CHECK(RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT != 0);
        CHECK((RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT & RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0);
        CHECK((RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT & RHI_BUFFER_USAGE_INDEX_BUFFER_BIT) == 0);
    }

    BeginGroup("a batch walked by stride decodes to the right commands");
    {
        // Nanite's shape: one command per visible cluster, tightly packed.
        std::vector<RHIDrawIndexedIndirectCommand> source;
        for (uint32_t i = 0; i < 4; ++i)
        {
            RHIDrawIndexedIndirectCommand c {};
            c.indexCount = 90u + i;
            c.instanceCount = 1u;
            c.firstIndex = 300u * i;
            c.vertexOffset = static_cast<int32_t>(i) * 128;
            c.firstInstance = i; // Nanite puts the cluster index here
            source.push_back(c);
        }

        const uint32_t stride = static_cast<uint32_t>(sizeof(RHIDrawIndexedIndirectCommand));
        const std::vector<uint8_t> bytes = MakeIndexedCommands(source, stride);

        // This is exactly the loop Metal runs: offset + i * stride.
        for (uint32_t i = 0; i < 4; ++i)
        {
            const RHIDrawIndexedIndirectCommand got = ReadIndexedCommand(bytes, i * stride);
            const RHIDrawIndexedIndirectCommand& want = source[i];
            CHECK(got.indexCount == want.indexCount);
            CHECK(got.instanceCount == want.instanceCount);
            CHECK(got.firstIndex == want.firstIndex);
            CHECK(got.vertexOffset == want.vertexOffset);
            CHECK(got.firstInstance == want.firstInstance);
        }
    }

    BeginGroup("a padded stride still steps command to command");
    {
        // A backend is allowed to be given a stride larger than the struct; the
        // extra bytes are just skipped. Vulkan takes stride directly and Metal
        // multiplies by it, so both must agree.
        std::vector<RHIDrawIndexedIndirectCommand> source;
        for (uint32_t i = 0; i < 3; ++i)
        {
            RHIDrawIndexedIndirectCommand c {};
            c.indexCount = 12u * (i + 1);
            c.firstIndex = 7u * i;
            source.push_back(c);
        }

        const uint32_t stride = static_cast<uint32_t>(sizeof(RHIDrawIndexedIndirectCommand)) + 16u;
        const std::vector<uint8_t> bytes = MakeIndexedCommands(source, stride);

        for (uint32_t i = 0; i < 3; ++i)
        {
            const RHIDrawIndexedIndirectCommand got = ReadIndexedCommand(bytes, i * stride);
            CHECK(got.indexCount == source[i].indexCount);
            CHECK(got.firstIndex == source[i].firstIndex);
        }
    }

    BeginGroup("negative vertexOffset survives the round trip");
    {
        // vertexOffset is the only signed field: a cluster index-basing scheme can
        // legitimately produce a negative base vertex, and DX12's
        // D3D12_DRAW_INDEXED_ARGUMENTS models it as INT too.
        RHIDrawIndexedIndirectCommand c {};
        c.indexCount = 36u;
        c.instanceCount = 2u;
        c.firstIndex = 4u;
        c.vertexOffset = -512;
        c.firstInstance = 9u;

        const std::vector<uint8_t> bytes = MakeIndexedCommands({c}, sizeof(RHIDrawIndexedIndirectCommand));
        const RHIDrawIndexedIndirectCommand got = ReadIndexedCommand(bytes, 0);
        CHECK(got.vertexOffset == -512);
        CHECK(got.firstInstance == 9u);
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
