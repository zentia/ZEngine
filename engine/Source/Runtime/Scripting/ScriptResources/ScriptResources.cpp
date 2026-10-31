#include "Runtime/Scripting/ScriptResources/ScriptResources.h"

#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    /// 缓存。函数内 static 而不是文件级全局：后者会在 `main()` 之前构造，而它的
    /// 析构又可能晚于其它全局对象（`Object` 的注册表就是），顺序错了就是退出时
    /// 崩在静态析构里 —— 那种崩溃看不到任何有用的栈。
    using ResourceCache = std::unordered_map<std::string, Object*>;

    ResourceCache& Cache()
    {
        static ResourceCache cache;
        return cache;
    }

    /// 不带扩展名时逐个补的扩展名，按"最可能是这个"排序。
    ///
    /// `.zasset` 排第一：它是导入产物（cooked），运行时真正读的就是它；`.mat`
    /// `.prefab` 是 YAML  authored 资产，编辑器里和玩家包里都可能有。
    const char* const kProbExtensions[] = {".zasset", ".mat", ".prefab", ".json", ".scene", ".timeline"};

    std::vector<std::filesystem::path> SearchRoots()
    {
        std::vector<std::filesystem::path> roots;

        const ConfigManager* config = GET_SYSTEM(ConfigManager);
        if (config != nullptr)
        {
            if (!config->GetAssetFolder().empty())
            {
                roots.push_back(config->GetAssetFolder());
            }
            if (!config->GetRootFolder().empty())
            {
                roots.push_back(config->GetRootFolder());
                roots.push_back(config->GetRootFolder() / "Assets");
                // 本引擎自己那份素材在 `engine/asset/`（不是 `Assets/`）：只认
                // Unity 的拼写会让仓库里所有现成素材都"找不到"。
                roots.push_back(config->GetRootFolder() / "asset");
            }
        }

        const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
        if (project != nullptr && !project->GetProjectContent().empty())
        {
            roots.push_back(project->GetProjectContent());
        }

        return roots;
    }

    /// 试 `base` 与 `base + ext`，命中就返回绝对规范化后的路径。
    std::filesystem::path TryCandidate(const std::filesystem::path& base)
    {
        std::error_code ec;
        if (std::filesystem::exists(base, ec))
        {
            return std::filesystem::absolute(base).lexically_normal();
        }
        for (const char* extension : kProbExtensions)
        {
            std::filesystem::path with_extension = base;
            with_extension += extension;
            if (std::filesystem::exists(with_extension, ec))
            {
                return std::filesystem::absolute(with_extension).lexically_normal();
            }
        }
        return std::filesystem::path();
    }

    /// 把素材路径解析成一个存在的绝对路径；解析不到返回空路径。
    std::filesystem::path ResolveAssetPath(const eastl::string& path)
    {
        if (path.empty())
        {
            return std::filesystem::path();
        }

        const std::filesystem::path relative(path.c_str());

        // 绝对路径不查搜索根：调用方已经说清楚是哪儿了，再套一层 root 只会把
        // `D:/x/a.json` 变成 `<root>/D:/x/a.json` 这种永远不存在的路径。
        if (relative.is_absolute())
        {
            return TryCandidate(relative);
        }

        for (const std::filesystem::path& root : SearchRoots())
        {
            // Unity 的约定：先找 `Resources/` 子目录。放在每个 root 内部循环而
            // 不是外循环，是为了让"工程自己的 Resources"优先于"引擎根的"。
            for (const char* prefix : {"Resources", ""})
            {
                std::filesystem::path base = *prefix == '\0' ? (root / relative) : (root / prefix / relative);
                const std::filesystem::path found = TryCandidate(base);
                if (!found.empty())
                {
                    return found;
                }
            }
        }

        return std::filesystem::path();
    }

    /// 缓存键。用解析后的绝对路径，且 Windows 上统一小写 —— NTFS 大小写不敏感，
    /// `A/b.zasset` 和 `a/b.zasset` 是同一个文件，按原拼写缓存会读出两份对象。
    std::string CacheKey(const std::filesystem::path& resolved)
    {
        std::string key = resolved.generic_string();
#if defined(_WIN32)
        for (char& character : key)
        {
            if (character >= 'A' && character <= 'Z')
            {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
#endif
        return key;
    }
}  // namespace

Object* Resources::Load(const eastl::string& path)
{
    const std::filesystem::path resolved = ResolveAssetPath(path);
    if (resolved.empty())
    {
        LOG_ERROR(ZAsset, "Resources.Load('{}'): no such asset (searched Resources/ and the asset roots)", path.c_str());
        return nullptr;
    }

    const std::string key = CacheKey(resolved);
    ResourceCache& cache = Cache();
    const ResourceCache::const_iterator cached = cache.find(key);
    if (cached != cache.end())
    {
        return cached->second;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZAsset, "Resources.Load('{}'): AssetManager is not registered", path.c_str());
        return nullptr;
    }

    // 类型只能由**文件自己**给出。
    //
    // `AssetManager::loadAsset<Object>()` 这条退路看着更通用，实际走不通：它按
    // 扩展名分派，`.json` 落到 `FileSystem::loadAssetJson<Object>()`，而后者要调
    // `Object::Transfer()` —— 那是 protected。于是 `loadAsset<Object>` 从来就没
    // 被实例化过（模板不实例化就不报这个错），一写才暴露。
    //
    // 退一步说这条退路也不该有：解出一个裸 `Object` 对脚本毫无意义（没有任何
    // 成员），而"素材类型由文件声明"正是 `.zasset` / `.mat` / `.prefab` 都已有
    // 的约定。
    const std::string type_name = asset_manager->GetAssetTypeName(resolved);
    if (type_name.empty())
    {
        LOG_ERROR(ZAsset,
                  "Resources.Load('{}'): the file declares no asset type, so it cannot be decoded "
                  "(a bare .json has no type stamped in it -- cook it to .zasset first)",
                  path.c_str());
        return nullptr;
    }

    const Type* type = TypeManager::GetInstance().ClassNameToType(type_name.c_str());
    if (type == nullptr)
    {
        LOG_ERROR(ZAsset,
                  "Resources.Load('{}'): the file declares type '{}' but no such class is registered",
                  path.c_str(),
                  type_name);
        return nullptr;
    }

    std::filesystem::path mutable_path = resolved;  // ReadObject 吃非常量引用
    Object* object = asset_manager->ReadObject(mutable_path, type);
    if (object == nullptr)
    {
        LOG_ERROR(ZAsset, "Resources.Load('{}'): the file exists but could not be decoded", path.c_str());
        return nullptr;
    }

    cache.emplace(key, object);
    return object;
}

Object* Resources::LoadTyped(const eastl::string& path, const eastl::string& type_name)
{
    if (type_name.empty())
    {
        return Load(path);
    }

    const Type* type = TypeManager::GetInstance().ClassNameToType(type_name.c_str());
    if (type == nullptr)
    {
        LOG_ERROR(ZAsset, "Resources.LoadTyped('{}', '{}'): no such class is registered", path.c_str(), type_name.c_str());
        return nullptr;
    }

    const std::filesystem::path resolved = ResolveAssetPath(path);
    if (resolved.empty())
    {
        LOG_ERROR(ZAsset, "Resources.LoadTyped('{}'): no such asset", path.c_str());
        return nullptr;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZAsset, "Resources.LoadTyped('{}'): AssetManager is not registered", path.c_str());
        return nullptr;
    }

    const std::string key = CacheKey(resolved) + "|" + type_name.c_str();
    ResourceCache& cache = Cache();
    const ResourceCache::const_iterator cached = cache.find(key);
    if (cached != cache.end())
    {
        return cached->second;
    }

    std::filesystem::path mutable_path = resolved;
    Object* object = asset_manager->ReadObject(mutable_path, type);
    if (object == nullptr)
    {
        LOG_ERROR(ZAsset,
                  "Resources.LoadTyped('{}', '{}'): the file exists but is not of that type",
                  path.c_str(),
                  type_name.c_str());
        return nullptr;
    }

    cache.emplace(key, object);
    return object;
}

bool Resources::Exists(const eastl::string& path)
{
    return !ResolveAssetPath(path).empty();
}

eastl::string Resources::GetResolvedPath(const eastl::string& path)
{
    const std::filesystem::path resolved = ResolveAssetPath(path);
    return resolved.empty() ? eastl::string() : eastl::string(resolved.generic_string().c_str());
}

eastl::string Resources::GetTypeName(Object* asset)
{
    return (asset != nullptr) ? eastl::string(asset->GetTypeName()) : eastl::string();
}

bool Resources::UnloadAsset(Object* asset)
{
    if (asset == nullptr)
    {
        return false;
    }

    ResourceCache& cache = Cache();
    for (ResourceCache::iterator it = cache.begin(); it != cache.end(); ++it)
    {
        if (it->second == asset)
        {
            cache.erase(it);
            return true;
        }
    }
    return false;
}

int Resources::UnloadUnusedAssets()
{
    ResourceCache& cache = Cache();
    const int dropped = static_cast<int>(cache.size());
    cache.clear();
    return dropped;
}

int Resources::GetLoadedCount()
{
    return static_cast<int>(Cache().size());
}
