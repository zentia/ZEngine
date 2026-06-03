#include "DerivedDataCacheAccessor.h"

#include "LMDBDerivedDataCache.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"

#include <cstdio>
#include <memory>
#include <mutex>

namespace Runtime
{
namespace
{
    std::mutex g_ddc_mutex;
    std::unique_ptr<LMDBDerivedDataCache> g_ddc;          // backing store (may be null)
    std::filesystem::path g_ddc_path;                     // currently-open dir
    bool g_ddc_open_attempted_for_project = false;        // avoid retrying a failed project open every call

    // Open `dir` into g_ddc. Caller holds g_ddc_mutex. Returns the live cache or
    // nullptr. If the same dir is already open, reuses it.
    IDerivedDataCache* OpenLocked(const std::filesystem::path& dir, size_t max_size_mb)
    {
        if (g_ddc && g_ddc_path == dir)
        {
            return g_ddc.get();
        }

        // Different path requested -> tear down the old env first.
        if (g_ddc)
        {
            g_ddc->Shutdown();
            g_ddc.reset();
            g_ddc_path.clear();
        }

        if (dir.empty())
        {
            return nullptr;
        }

        auto cache = std::make_unique<LMDBDerivedDataCache>();
        if (!cache->Initialize(dir, max_size_mb))
        {
            return nullptr;
        }

        g_ddc = std::move(cache);
        g_ddc_path = dir;
        return g_ddc.get();
    }
}  // namespace

IDerivedDataCache* GetDerivedDataCache()
{
    std::lock_guard<std::mutex> lock(g_ddc_mutex);

    if (g_ddc)
    {
        return g_ddc.get();
    }

    if (g_ddc_open_attempted_for_project)
    {
        return nullptr;  // already failed once this project; don't thrash
    }

    std::filesystem::path ddc_root;
    if (const auto project = SystemRegistry::GetInstance().GetSystem<ProjectInfo>())
    {
        ddc_root = project->GetIntermediateDDCRoot();
    }

    g_ddc_open_attempted_for_project = true;
    if (ddc_root.empty())
    {
        return nullptr;
    }
    return OpenLocked(ddc_root, /*max_size_mb*/ 1024);
}

IDerivedDataCache* OpenDerivedDataCacheAt(const std::filesystem::path& dir, size_t max_size_mb)
{
    std::lock_guard<std::mutex> lock(g_ddc_mutex);
    g_ddc_open_attempted_for_project = false;  // explicit open resets the project-retry guard
    return OpenLocked(dir, max_size_mb);
}

void ShutdownDerivedDataCache()
{
    std::lock_guard<std::mutex> lock(g_ddc_mutex);
    if (g_ddc)
    {
        g_ddc->Shutdown();
        g_ddc.reset();
    }
    g_ddc_path.clear();
    g_ddc_open_attempted_for_project = false;
}

std::string MakeDDCCacheKey(const std::string& platform_tag, uint64_t settings_hash, uint32_t encoder_version)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s_%016llx_v%u",
                  platform_tag.c_str(),
                  static_cast<unsigned long long>(settings_hash),
                  encoder_version);
    return std::string(buf);
}
}  // namespace Runtime
