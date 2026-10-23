#include "Runtime/Function/Render/Spine/SpineGpuAssetFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace
{
    std::string LowerFileName(const std::filesystem::path& p)
    {
        std::string name = SpineGpuPathToUtf8(p.filename());
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name;
    }

    bool EndsWith(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }
}  // namespace

std::string SpineGpuPathToUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path SpineGpuPathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

bool SpineGpuIsContainerPath(const std::filesystem::path& path)
{
    return EndsWith(LowerFileName(path), ".zspg");
}

std::filesystem::path SpineGpuStripSkeletonSuffix(const std::filesystem::path& skeleton_path)
{
    const std::string lower = LowerFileName(skeleton_path);
    const std::string name = SpineGpuPathToUtf8(skeleton_path.filename());
    for (const char* suf : {".skel.bytes", ".skel", ".json"})
    {
        if (EndsWith(lower, suf))
        {
            return skeleton_path.parent_path() / SpineGpuPathFromUtf8(name.substr(0, name.size() - std::strlen(suf)));
        }
    }
    return skeleton_path;
}

bool SpineGpuFindAtlas(const std::filesystem::path& skeleton_path, std::filesystem::path& out_atlas)
{
    const std::filesystem::path stem = SpineGpuStripSkeletonSuffix(skeleton_path);
    std::error_code ec;
    for (const char* ext : {".atlas", ".atlas.txt"})
    {
        std::filesystem::path candidate = stem;
        candidate += ext;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec)
        {
            out_atlas = candidate;
            return true;
        }
    }
    return false;
}

bool SpineGpuIsSkeletonSourcePath(const std::filesystem::path& path)
{
    const std::string lower = LowerFileName(path);
    if (EndsWith(lower, ".skel.bytes") || EndsWith(lower, ".skel"))
    {
        return true;
    }
    if (EndsWith(lower, ".json"))
    {
        // json 太常见（工程配置 / 材质 / 场景旁的描述文件）：只有同名图集在时才当骨架
        std::filesystem::path atlas;
        return SpineGpuFindAtlas(path, atlas);
    }
    return false;
}

std::filesystem::path SpineGpuCookedPathFor(const std::filesystem::path& skeleton_path)
{
    std::filesystem::path out = SpineGpuStripSkeletonSuffix(skeleton_path);
    out += ".zspg";
    return out;
}

bool SpineGpuReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    // std::filesystem::path 在 MSVC 上走宽字符打开：中文目录也能读（stbi / fopen 的 ANSI 路径不行）
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0)
    {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    if (n > 0)
    {
        f.read(reinterpret_cast<char*>(out.data()), n);
    }
    return static_cast<bool>(f) || n == 0;
}

SpineImportResult SpineGpuCookSkeleton(const std::filesystem::path& skeleton_path,
                                       const std::filesystem::path& atlas_path, std::vector<uint8_t>& container_out,
                                       SpineGpuAsset* asset_out)
{
    SpineImportResult r;
    std::vector<uint8_t> atlas_bytes;
    if (!SpineGpuReadFile(atlas_path, atlas_bytes))
    {
        r.error = "读不到图集 " + SpineGpuPathToUtf8(atlas_path);
        return r;
    }
    std::vector<uint8_t> skel_bytes;
    if (!SpineGpuReadFile(skeleton_path, skel_bytes))
    {
        r.error = "读不到骨架 " + SpineGpuPathToUtf8(skeleton_path);
        return r;
    }
    SpineAtlas atlas;
    r = ParseSpineAtlas(reinterpret_cast<const char*>(atlas_bytes.data()), atlas_bytes.size(), atlas);
    if (!r.ok)
    {
        r.error = "图集 " + SpineGpuPathToUtf8(atlas_path.filename()) + "：" + r.error;
        return r;
    }
    // ⚠️ 按**扩展名**选解析器，不看内容：.skel 没魔数也没长度字段，猜错就是越界读
    SpineSourceSkeleton src;
    const std::string lower = LowerFileName(skeleton_path);
    if (EndsWith(lower, ".json"))
    {
        r = ParseSpineSkeletonJson(reinterpret_cast<const char*>(skel_bytes.data()), skel_bytes.size(), &atlas, src);
    }
    else if (EndsWith(lower, ".skel") || EndsWith(lower, ".skel.bytes"))
    {
        r = ParseSpineSkeletonBinary(skel_bytes.data(), skel_bytes.size(), &atlas, src);
    }
    else
    {
        r.ok = false;
        r.error = "认不出骨架类型（只认 .json / .skel / <名>.skel.bytes）：" + SpineGpuPathToUtf8(skeleton_path);
        return r;
    }
    if (!r.ok)
    {
        r.error = "骨架 " + SpineGpuPathToUtf8(skeleton_path.filename()) + "：" + r.error;
        return r;
    }
    SpineGpuAsset baked;
    r = BakeSpineGpuAsset(src, baked);
    if (!r.ok)
    {
        r.error = "烘焙 " + SpineGpuPathToUtf8(skeleton_path.filename()) + "：" + r.error;
        return r;
    }
    container_out.clear();
    r = WriteSpineGpuContainer(baked, container_out);
    if (!r.ok)
    {
        r.error = "序列化 " + SpineGpuPathToUtf8(skeleton_path.filename()) + "：" + r.error;
        return r;
    }
    if (asset_out != nullptr)
    {
        r = ReadSpineGpuContainer(container_out.data(), container_out.size(), *asset_out);
    }
    return r;
}

SpineImportResult SpineGpuLoadAssetFile(const std::filesystem::path& path, SpineGpuAsset& out,
                                        std::filesystem::path& page_dir_out)
{
    SpineImportResult r;
    if (SpineGpuIsContainerPath(path))
    {
        std::vector<uint8_t> bytes;
        if (!SpineGpuReadFile(path, bytes))
        {
            r.error = "读不到容器 " + SpineGpuPathToUtf8(path);
            return r;
        }
        r = ReadSpineGpuContainer(bytes.data(), bytes.size(), out);
        if (!r.ok)
        {
            r.error = "容器 " + SpineGpuPathToUtf8(path.filename()) + "：" + r.error;
            return r;
        }
        page_dir_out = path.parent_path();
        return r;
    }
    std::filesystem::path atlas;
    if (!SpineGpuFindAtlas(path, atlas))
    {
        r.error = "找不到同名图集（" + SpineGpuPathToUtf8(SpineGpuStripSkeletonSuffix(path)) + ".atlas / .atlas.txt）";
        return r;
    }
    std::vector<uint8_t> container;
    r = SpineGpuCookSkeleton(path, atlas, container, &out);
    if (r.ok)
    {
        page_dir_out = atlas.parent_path();
    }
    return r;
}

std::shared_ptr<const SpineGpuAssetInfo> SpineGpuGetAssetInfo(const std::filesystem::path& path)
{
    struct Entry
    {
        std::filesystem::file_time_type mtime {};
        uintmax_t size = 0;
        std::shared_ptr<const SpineGpuAssetInfo> info;
    };
    static std::mutex s_mutex;
    static std::map<std::string, Entry> s_cache;

    std::error_code ec;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, ec);
    const uintmax_t size = ec ? 0 : std::filesystem::file_size(path, ec);
    const std::string key = SpineGpuPathToUtf8(path);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const auto it = s_cache.find(key);
        if (it != s_cache.end() && !ec && it->second.mtime == mtime && it->second.size == size)
        {
            return it->second.info;
        }
    }
    auto info = std::make_shared<SpineGpuAssetInfo>();
    if (ec)
    {
        info->error = "文件不存在：" + key;
        return info;   // 不缓存：文件可能马上就出现（cook 中）
    }
    SpineGpuAsset asset;
    std::filesystem::path page_dir;
    const SpineImportResult r = SpineGpuLoadAssetFile(path, asset, page_dir);
    if (!r.ok)
    {
        info->error = r.error;
    }
    else
    {
        info->animations = asset.anim_names;
        info->durations.reserve(asset.anims.size());
        for (const SpineGpuAnim& a : asset.anims)
        {
            info->durations.push_back(a.duration);
        }
        info->skins = asset.skin_names;
        info->event_strings = asset.event_strings;
        for (const SpineGpuEventData& e : asset.events)
        {
            info->event_names.push_back(e.name < asset.event_strings.size() ? asset.event_strings[e.name] : std::string());
            info->event_audio.push_back(e.audio_path < asset.event_strings.size() ? asset.event_strings[e.audio_path]
                                                                                    : std::string());
        }
        info->bone_count = asset.header.bone_count;
        info->slot_count = asset.header.slot_count;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[key] = Entry {mtime, size, info};
    return info;
}
