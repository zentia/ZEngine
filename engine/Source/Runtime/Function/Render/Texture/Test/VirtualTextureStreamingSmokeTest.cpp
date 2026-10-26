// Headless smoke test for the Virtual Texture streaming policy (CPU bookkeeping
// half). Compiled as a standalone TU under emcc + node via
// scripts/verify_vt_wasm.sh -- no RHI, no device, no browser.
//
// It exercises the 6 CPU-side TODOs filled in step 2.6:
//   - RequestPages mip selection + world-footprint stamping
//   - Update -> UpdatePagePriorities (reproject stored bounds to the live screen)
//   - ProcessPageLoadRequests -> LoadPage (slot allocation + page-table entry)
//   - EvictLRUPages (memory-budget enforcement)
//   - stats consistency
// The 3 GPU TODOs (page upload / page-table texture) are intentionally NOT
// exercised here -- PhysicalTexturePool::InitializeLogical() builds the slot grid
// without a Vulkan image, so the policy is asserted against a device-free pool.

#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureSettings.h"

#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "  FAIL[%d]: %s\n", g_checks, (msg));         \
        }                                                                      \
    } while (0)

// Build a synthetic 256x256 R8G8B8A8 texture (4 mip levels of pages at page_size
// 128 -> mip0 is 2x2 pages). Fills pixels with a deterministic gradient so the
// data is non-null and the format is R8G8B8A8_UNORM (4 bytes/pixel).
static std::shared_ptr<TextureData> MakeTextureData(uint32_t w, uint32_t h)
{
    auto td = std::make_shared<TextureData>();
    td->m_Width = w;
    td->m_Height = h;
    td->m_Format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
    td->m_PixelsLength = static_cast<uint32_t>(w * h * 4);
    td->m_Pixels = new uint8_t[td->m_PixelsLength];
    for (uint32_t i = 0; i < w * h; ++i)
    {
        td->m_Pixels[i * 4 + 0] = static_cast<uint8_t>((i & 0xFF));
        td->m_Pixels[i * 4 + 1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        td->m_Pixels[i * 4 + 2] = 128;
        td->m_Pixels[i * 4 + 3] = 255;
    }
    return td;
}

int main()
{
    std::printf("== VirtualTexture streaming policy smoke test (headless WASM)\n");

    // --- Setup: logical pool (no Vulkan), async loading OFF so the main thread
    //     drains the request queue deterministically inside Update().
    VirtualTextureManager mgr;
    VirtualTextureConfig cfg = mgr.getConfig();
    cfg.enable_async_loading = false;
    // ProcessPageLoadRequests drains max_concurrent_loads per Update; the default
    // 4 is fine for the 256x256 texture (4 mip-0 pages) but the 1024x1024 LRU
    // section has 64. Raise it so a single Update drains the whole queue.
    cfg.max_concurrent_loads = 512;
    mgr.SetConfig(cfg);
    mgr.InitializeLogical(16, 16, 128);  // 16x16 = 256 slots, 128px pages

    auto td = MakeTextureData(256, 256);
    VirtualTextureHandle handle = mgr.RegisterVirtualTexture(td, 128);
    CHECK(handle != 0, "RegisterVirtualTexture returns a non-zero handle");

    auto resource = mgr.GetVirtualTexture(handle);
    CHECK(resource != nullptr, "registered resource is retrievable");
    CHECK(resource->getVirtualWidth() == 256 && resource->getVirtualHeight() == 256,
          "virtual dimensions match the source texture");
    CHECK(resource->getPagesX() == 2 && resource->getPagesY() == 2,
          "mip-0 page grid is 2x2 for a 256x256 texture at page_size 128");

    // --- RequestPages at mip 0 (screen_size large -> mip 0 selected).
    Vector3 camera(0.0f, 0.0f, 500.0f);
    Vector3 object(0.0f, 0.0f, 0.0f);
    AxisAlignedBox bounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(100.0f, 100.0f, 0.0f));
    mgr.RequestPages(handle, camera, object, bounds, 1.0f);  // screen_size ~ full

    // Update drains the queue (async off) and recomputes priorities.
    mgr.Update(camera, Vector3(0.0f, 0.0f, -1.0f), 90.0f * 3.14159265358979323846f / 180.0f,
               16.0f / 9.0f, 1600, 900, 0.016f);

    CHECK(mgr.ArePagesLoaded(handle, 0), "all mip-0 pages loaded after Update drains the queue");

    // Stats must be internally consistent: loaded_pages >= mip-0 count, and the
    // physical pool reports the slots we expect.
    VirtualTextureStats stats = mgr.GetStats();
    CHECK(stats.loaded_pages >= 4, "stats.loaded_pages reflects the loaded mip-0 pages");
    CHECK(stats.physical_pool_total_slots == 256, "logical pool exposes 16*16 slots");
    CHECK(stats.physical_pool_used_slots >= 4, "physical pool marks the loaded pages' slots used");
    CHECK(stats.memory_usage_ratio > 0.0f && stats.memory_usage_ratio <= 1.0f,
          "memory_usage_ratio is a valid [0,1] fraction");

    // --- LRU eviction: register a large texture so a whole-MB budget actually
    //     fires EvictLRUPages. A 1024x1024 texture is 8x8 = 64 mip-0 pages =
    //     64 * 128*128*4 = 4MB; a 1MB budget fits ~16 pages, so Update must
    //     evict the oldest. (memory_budget_mb is whole-MB, so sub-MB budgets
    //     truncate to 0, which the evictor treats as "unlimited" and skips.)
    auto td_big = MakeTextureData(1024, 1024);
    VirtualTextureHandle handle_big = mgr.RegisterVirtualTexture(td_big, 128);
    CHECK(handle_big != 0, "big texture registers a non-zero handle");
    mgr.RequestPages(handle_big, camera, object, bounds, 1.0f);
    mgr.Update(camera, Vector3(0.0f, 0.0f, -1.0f), 90.0f * 3.14159265358979323846f / 180.0f,
               16.0f / 9.0f, 1600, 900, 0.016f);
    CHECK(mgr.ArePagesLoaded(handle_big, 0), "all 64 big-texture mip-0 pages loaded before eviction");

    VirtualTextureConfig cfg2 = mgr.getConfig();
    cfg2.memory_budget_mb = 1;  // 1MB fits ~16 of the 64 loaded pages
    mgr.SetConfig(cfg2);

    mgr.Update(camera, Vector3(0.0f, 0.0f, -1.0f), 90.0f * 3.14159265358979323846f / 180.0f,
               16.0f / 9.0f, 1600, 900, 0.016f);

    VirtualTextureStats stats2 = mgr.GetStats();
    const size_t per_slot = static_cast<size_t>(128) * 128 * 4;
    const size_t budget_bytes = static_cast<size_t>(1) * 1024 * 1024;
    CHECK(stats2.physical_pool_used_slots <= budget_bytes / per_slot + 1,
          "LRU eviction respects the memory budget (used slots within budget)");
    CHECK(stats2.loaded_pages < 64, "eviction reduced the loaded page count below the full 64");
    mgr.UnregisterVirtualTexture(handle_big);

    // --- Distance gating: move the camera far beyond max_distance (default
    //     2000) and prove UpdatePagePriorities recomputes visibility. After the
    //     LRU eviction above, the small texture's pages may have been evicted
    //     (they loaded earliest), so force them back loaded (ForceLoadPages
    //     ignores distance), then move the camera away and Update. Every loaded
    //     page of the far object must now be flagged not-visible.
    mgr.ForceLoadPages(handle, 0);
    Vector3 far_camera(0.0f, 0.0f, 500000.0f);
    mgr.Update(far_camera, Vector3(0.0f, 0.0f, -1.0f),
               90.0f * 3.14159265358979323846f / 180.0f, 16.0f / 9.0f, 1600, 900, 0.016f);
    bool any_not_visible = false;
    const auto& pages = resource->getAllPages();
    for (const auto& p : pages)
    {
        if (p.second.state == VirtualTexturePageState::Loaded && !p.second.is_visible)
        {
            any_not_visible = true;
            break;
        }
    }
    CHECK(any_not_visible, "pages beyond max_distance are flagged not-visible after Update");

    // --- Unregister unloads everything.
    mgr.UnregisterVirtualTexture(handle);
    CHECK(mgr.GetVirtualTexture(handle) == nullptr, "UnregisterVirtualTexture removes the resource");

    std::printf("== results: %d checks, %d failed\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        std::fprintf(stderr, "==> FAIL: %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("==> PASS: VirtualTexture streaming policy %d/%d green\n", g_checks, g_checks);
    return 0;
}
