#include "Runtime/Scripting/ScriptScene/ScriptScene.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Framework/Level/Level.h"

#include <EASTL/unordered_map.h>
#include <memory>
#include <vector>

// Scene 是"视图"不是所有者：它指向一张 Level，由 SceneManager 缓存复用。
// 为什么不每次查询都 new？脚本里 `SceneManager.GetActiveScene().name`
// 写在 Update 里是很常见的写法，每帧 new 一个 Object 就是每帧漏一个。
namespace
{
    /// 按 Level* 缓存 Scene 实例。
    ///
    /// 为什么按 Level* 而不是按 URL 建：Level 会被**重载**（同一个 URL 第二次
    /// 加载是另一个 Level 实例）。按 URL 缓存的话，重载后脚本手里的 Scene 还
    /// 指向旧的、已经 Unload 过的那张 —— IsLoaded() 变成 false，根对象列表是空的，
    /// 而脚本完全不知道发生了什么。按指针缓存则同一个 Scene 对象会自动跟着换
    /// 指向，脚本持有的引用不会失效。
    eastl::unordered_map<Level*, std::unique_ptr<Scene>>& SceneCache()
    {
        static eastl::unordered_map<Level*, std::unique_ptr<Scene>> cache;
        return cache;
    }

    Scene* SceneFor(Level* level)
    {
        if (level == nullptr)
        {
            return nullptr;
        }

        auto& cache = SceneCache();
        const auto found = cache.find(level);
        if (found != cache.end())
        {
            return found->second.get();
        }

        auto created = std::make_unique<Scene>();
        created->Bind(level);
        // InstanceID 为 0 的 Object 对 PPtr 等于"不存在"。这里 Scene 是走
        // Converter<Scene*> 直接包给 JS 的，不经 PPtr，但保持一致性更省心 ——
        // 而且 ObjectManager 缺席（早期 / 工具进程）时也不能因此失败。
        if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
        {
            object_manager->AllocateAndAssignInstanceID(created.get());
        }

        Scene* result = created.get();
        cache.emplace(level, std::move(created));
        return result;
    }

    WorldManager* World()
    {
        return GET_SYSTEM(WorldManager);
    }

    /// "Assets/Scenes/Main.level" -> "Main"。
    ///
    /// 取最后一个路径分隔符之后的部分，再去掉最后一个 '.' 之后的内容。
    /// 没有扩展名就原样返回（"Assets/Scenes/Main" -> "Main"）。
    eastl::string SceneNameFromUrl(const eastl::string& url)
    {
        const eastl::string::size_type slash = url.find_last_of("/\\");
        eastl::string stem = (slash == eastl::string::npos) ? url : url.substr(slash + 1);
        const eastl::string::size_type dot = stem.find_last_of('.');
        if (dot != eastl::string::npos && dot > 0)
        {
            stem = stem.substr(0, dot);
        }
        return stem;
    }

    /// 一张 Level 的根对象：Transform 没有父节点的那些。
    ///
    /// 两张表都要看（同 GameObject::FindByName 的理由）：关卡对象表是
    /// shared_ptr，运行时生成的对象（Instantiate 产物）在另一张非拥有表上。
    std::vector<GameObject*> RootsOf(const Level& level)
    {
        std::vector<GameObject*> roots;

        const auto consider = [&roots](GameObject* game_object) {
            if (game_object == nullptr)
            {
                return;
            }
            Transform* transform = game_object->GetTransform();
            if (transform == nullptr || transform->GetParent() == nullptr)
            {
                roots.push_back(game_object);
            }
        };

        for (const auto& entry : level.getAllGObjects())
        {
            consider(entry.second.get());
        }
        for (const auto& entry : level.GetRuntimeGObjects())
        {
            consider(entry.second);
        }
        return roots;
    }
}  // namespace

// ---------------------------------------------------------------- Scene ----

eastl::string Scene::GetName() const
{
    if (m_Level == nullptr)
    {
        return eastl::string();
    }
    return SceneNameFromUrl(m_Level->getLevelResUrl());
}

eastl::string Scene::GetPath() const
{
    if (m_Level == nullptr)
    {
        return eastl::string();
    }
    return m_Level->getLevelResUrl();
}

bool Scene::IsLoaded() const
{
    return m_Level != nullptr && m_Level->IsLoaded();
}

int Scene::GetRootCount() const
{
    if (m_Level == nullptr)
    {
        return 0;
    }
    return static_cast<int>(RootsOf(*m_Level).size());
}

std::vector<GameObject*> Scene::GetRootGameObjects() const
{
    if (m_Level == nullptr)
    {
        return {};
    }
    return RootsOf(*m_Level);
}

// --------------------------------------------------------- SceneManager ----

int SceneManager::GetSceneCount()
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return 0;
    }
    return static_cast<int>(world->GetLoadedLevelUrls().size());
}

Scene* SceneManager::GetSceneAt(int index)
{
    WorldManager* world = World();
    if (world == nullptr || index < 0)
    {
        return nullptr;
    }
    const eastl::vector<Level*> levels = world->GetLoadedLevels();
    if (index >= static_cast<int>(levels.size()))
    {
        return nullptr;
    }
    return SceneFor(levels[static_cast<size_t>(index)]);
}

Scene* SceneManager::GetActiveScene()
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return nullptr;
    }
    return SceneFor(world->getCurrentActiveLevel());
}

Scene* SceneManager::GetSceneByName(const eastl::string& name)
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return nullptr;
    }
    for (Level* level : world->GetLoadedLevels())
    {
        if (level != nullptr && SceneNameFromUrl(level->getLevelResUrl()) == name)
        {
            return SceneFor(level);
        }
    }
    return nullptr;
}

Scene* SceneManager::GetSceneByPath(const eastl::string& path)
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return nullptr;
    }
    return SceneFor(world->GetLoadedLevelByUrl(path));
}

bool SceneManager::IsSceneLoaded(const eastl::string& path)
{
    return GetSceneByPath(path) != nullptr;
}

bool SceneManager::LoadScene(const eastl::string& path)
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return false;
    }
    return world->OpenScene(path);
}

bool SceneManager::LoadSceneAdditive(const eastl::string& path)
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return false;
    }
    return world->LoadLevelAdditive(path);
}

bool SceneManager::UnloadScene(const eastl::string& path)
{
    WorldManager* world = World();
    if (world == nullptr)
    {
        return false;
    }

    // 拒绝卸载活动场景：那会让引擎进入"没有活动关卡"的状态，而 Level::Tick、
    // GameObject.Find、Instantiate 全都依赖它。Unity 会回退到一张空场景，这里
    // 选择明确报错 —— 静默地失去活动关卡比一个 false 难查得多。
    Level* target = world->GetLoadedLevelByUrl(path);
    if (target == nullptr)
    {
        return false;
    }
    if (target == world->getCurrentActiveLevel())
    {
        return false;
    }

    world->UnloadLevelByUrl(path);
    return true;
}

bool SceneManager::ReloadActiveScene()
{
    WorldManager* world = World();
    if (world == nullptr || world->getCurrentActiveLevel() == nullptr)
    {
        return false;
    }
    world->ReloadCurrentLevel();
    return true;
}
