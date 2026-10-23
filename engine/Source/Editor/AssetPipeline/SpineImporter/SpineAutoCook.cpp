#include "Editor/AssetPipeline/SpineImporter/SpineAutoCook.h"

#include "Runtime/Function/Framework/Component/Spine/SpineRenderer.h"
#include "Runtime/Function/Render/Spine/SpineGpuAssetFile.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace SpineAutoCook
{
    namespace
    {
        constexpr std::chrono::milliseconds kDebounce {300};

        std::mutex g_mutex;
        std::map<std::string, std::chrono::steady_clock::time_point> g_pending;   // 骨架源 → 最后一次事件的时刻

        std::string LowerName(const std::filesystem::path& p)
        {
            std::string n = SpineGpuPathToUtf8(p.filename());
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return n;
        }

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        // 图集改了 ⇒ 找同名骨架源（任一种）
        std::vector<std::filesystem::path> SkeletonsForAtlas(const std::filesystem::path& atlas)
        {
            const std::string lower = LowerName(atlas);
            const std::string name = SpineGpuPathToUtf8(atlas.filename());
            size_t cut = 0;
            if (EndsWith(lower, ".atlas.txt"))
            {
                cut = std::strlen(".atlas.txt");
            }
            else if (EndsWith(lower, ".atlas"))
            {
                cut = std::strlen(".atlas");
            }
            std::vector<std::filesystem::path> out;
            if (cut == 0)
            {
                return out;
            }
            const std::filesystem::path stem = atlas.parent_path() / SpineGpuPathFromUtf8(name.substr(0, name.size() - cut));
            std::error_code ec;
            for (const char* ext : {".skel.bytes", ".skel", ".json"})
            {
                std::filesystem::path candidate = stem;
                candidate += ext;
                if (std::filesystem::is_regular_file(candidate, ec) && SpineGpuIsSkeletonSourcePath(candidate))
                {
                    out.push_back(candidate);
                }
            }
            return out;
        }

        bool WriteAtomically(const std::filesystem::path& out, const std::vector<uint8_t>& bytes, std::string& error)
        {
            std::filesystem::path tmp = out;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f)
                {
                    error = "写不了 " + SpineGpuPathToUtf8(tmp);
                    return false;
                }
                f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!f)
                {
                    error = "写入被截断 " + SpineGpuPathToUtf8(tmp);
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, out, ec);   // 目标存在时替换（Windows 上是 MoveFileEx REPLACE_EXISTING）
            if (ec)
            {
                std::filesystem::remove(tmp, ec);
                error = "改名失败 " + SpineGpuPathToUtf8(out) + "：" + ec.message();
                return false;
            }
            return true;
        }
    }  // namespace

    bool IsStale(const std::filesystem::path& skeleton_path)
    {
        const std::filesystem::path out = SpineGpuCookedPathFor(skeleton_path);
        std::error_code ec;
        const auto out_time = std::filesystem::last_write_time(out, ec);
        if (ec)
        {
            return true;   // 没有产物
        }
        const auto skel_time = std::filesystem::last_write_time(skeleton_path, ec);
        if (ec || skel_time > out_time)
        {
            return true;
        }
        std::filesystem::path atlas;
        if (SpineGpuFindAtlas(skeleton_path, atlas))
        {
            const auto atlas_time = std::filesystem::last_write_time(atlas, ec);
            if (ec || atlas_time > out_time)
            {
                return true;
            }
        }
        return false;
    }

    bool CookOne(const std::filesystem::path& skeleton_path, std::string& error)
    {
        std::filesystem::path atlas;
        if (!SpineGpuFindAtlas(skeleton_path, atlas))
        {
            error = "找不到同名图集（" + SpineGpuPathToUtf8(SpineGpuStripSkeletonSuffix(skeleton_path)) +
                    ".atlas / .atlas.txt）";
            return false;
        }
        std::vector<uint8_t> container;
        const SpineImportResult r = SpineGpuCookSkeleton(skeleton_path, atlas, container, nullptr);
        if (!r.ok)
        {
            error = r.error;
            return false;
        }
        const std::filesystem::path out = SpineGpuCookedPathFor(skeleton_path);
        if (!WriteAtomically(out, container, error))
        {
            return false;
        }
        // 用这个 .zspg 的组件下一帧重建实例（渲染器按文件修改时间换新的一份资产）
        SpineRenderer::NotifyAssetChanged(SpineGpuPathToUtf8(std::filesystem::absolute(out).lexically_normal()));
        return true;
    }

    Stats CookProject(const std::filesystem::path& content_root)
    {
        Stats stats;
        std::error_code ec;
        if (content_root.empty() || !std::filesystem::is_directory(content_root, ec))
        {
            return stats;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (std::filesystem::recursive_directory_iterator it(content_root, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (!it->is_regular_file(ec) || !SpineGpuIsSkeletonSourcePath(it->path()))
            {
                continue;
            }
            ++stats.skeletons;
            if (!IsStale(it->path()))
            {
                ++stats.up_to_date;
                continue;
            }
            std::string error;
            if (CookOne(it->path(), error))
            {
                ++stats.cooked;
                LOG_INFO(ZAsset, "SpineAutoCook: {} -> {}", SpineGpuPathToUtf8(it->path()),
                         SpineGpuPathToUtf8(SpineGpuCookedPathFor(it->path()).filename()));
            }
            else
            {
                ++stats.failed;
                LOG_ERROR(ZAsset, "SpineAutoCook: {} failed: {}", SpineGpuPathToUtf8(it->path()), error);
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LOG_INFO(ZAsset, "SpineAutoCook: {} skeleton source(s) under {}: {} cooked, {} up to date, {} failed ({:.1f} ms)",
                 stats.skeletons, SpineGpuPathToUtf8(content_root), stats.cooked, stats.up_to_date, stats.failed, ms);
        return stats;
    }

    void Queue(const std::filesystem::path& changed_path)
    {
        std::vector<std::filesystem::path> skeletons;
        if (SpineGpuIsSkeletonSourcePath(changed_path))
        {
            skeletons.push_back(changed_path);
        }
        else
        {
            skeletons = SkeletonsForAtlas(changed_path);
        }
        if (skeletons.empty())
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const std::filesystem::path& s : skeletons)
        {
            g_pending[SpineGpuPathToUtf8(s)] = now;
        }
    }

    void Flush(std::chrono::steady_clock::time_point now)
    {
        std::vector<std::string> due;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (now - it->second >= kDebounce)
                {
                    due.push_back(it->first);
                    it = g_pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (const std::string& s : due)
        {
            const std::filesystem::path skel = SpineGpuPathFromUtf8(s);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(skel, ec))
            {
                continue;   // 改名 / 删除的中间态
            }
            std::string error;
            if (CookOne(skel, error))
            {
                LOG_INFO(ZAsset, "SpineAutoCook: re-cooked {}", s);
            }
            else
            {
                LOG_ERROR(ZAsset, "SpineAutoCook: re-cook {} failed: {}", s, error);
            }
        }
    }
}  // namespace SpineAutoCook
