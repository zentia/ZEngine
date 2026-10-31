#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/Object.h"

#include <EASTL/string.h>
#include <vector>

#include "ScriptScene.generated.h"

// GameObject 必须**完整定义**而不是前向声明：生成的 ScriptScene.gen.cpp 会为
// `std::vector<GameObject*>` 实例化 ZScript::Converter，而它的 T* 特化要求
// `std::is_base_of<Object, T>` —— 前向声明的类过不了这个 intrinsic，报的是
// "no ZScript::Converter for this type"，看不出真正的原因是缺 include。
class Level;

// 脚本侧的场景（Unity 的 `UnityEngine.SceneManagement.Scene` + `SceneManager`）。
//
// 与 Unity 的**一处刻意的差异**：没有 `buildIndex`
// ---------------------------------------------------
// Unity 的 `Scene.buildIndex` 读的是 Build Settings 里那份场景列表的下标。
// ZEngine 目前**没有**这样一份列表 —— ProjectInfo 和 PlayerSettings 里都没有，
// 打包流程也不读它。编一个下标出来（比如拿"已加载场景里的第几个"充当）会让
// 脚本依赖上一个含义完全不同的数字：Unity 里 buildIndex 是**固定的**，跨会话
// 稳定，可以按它写 `LoadScene(3)`；而已加载列表的下标随加载顺序变化。
// 宁可先不导出 —— 缺一个 API 是编译期报错，给个假值是运行期的错行为。
// 要补的时候从"给工程加一份有序的 Build Settings 场景列表"开始。
//
// `Scene` 不是值类型
// -------------------
// Unity 的 Scene 是 struct（值语义，`GetActiveScene()` 返回快照）。这里它是
// 一个 Object 派生类，由 SceneManager **缓存并复用**：每次查询都 new 一个的话，
// 每帧调 `GetActiveScene().name` 就会漏一整个对象。缓存按 Level* 建，所以
// 关卡重载后同一个 Scene 对象会自动指向新的状态 —— 脚本持有的引用不会失效。
// 代价是脚本拿到的是"活的"视图而不是快照，这与 Unity 不同，但更适合引擎侧。

ZCLASS()
class Scene : public Object
{
    ZGENERATED_BODY(Scene)

public:
    Scene() = default;

    /// 绑定到一张 Level。C++ 侧的装配入口，**不**导出给脚本：脚本只能从
    /// SceneManager 拿 Scene，不能自己造一个指向任意关卡的。
    void Bind(Level* level) { m_Level = level; }
    Level* GetLevel() const { return m_Level; }

    /// 场景名（URL 的文件名部分，去扩展名）。未绑定的 Scene 返回空串。
    ZFUNCTION()
    eastl::string GetName() const;

    /// 场景的资源 URL / 路径。
    ZFUNCTION()
    eastl::string GetPath() const;

    /// 这张 Scene 是否指向一张真实存在的 Level。
    ZFUNCTION()
    bool IsValid() const { return m_Level != nullptr; }

    /// Level 是否已完成加载。未绑定的 Scene 返回 false（不是"加载中"）。
    ZFUNCTION()
    bool IsLoaded() const;

    /// 根对象数（没有父 Transform 的对象）。
    ZFUNCTION()
    int GetRootCount() const;

    /// 根对象列表。Unity 的 `scene.GetRootGameObjects()`。
    ///
    /// 顺序**不确定**（来自 unordered_map 的遍历序）：需要稳定顺序就自己按
    /// name 或 GetID() 排。Unity 也不保证这个顺序。
    ZFUNCTION()
    std::vector<GameObject*> GetRootGameObjects() const;

    ZSCRIPTPROPERTY(Name="name", Getter="GetName")
    ZSCRIPTPROPERTY(Name="path", Getter="GetPath")
    ZSCRIPTPROPERTY(Name="rootCount", Getter="GetRootCount")
    ZSCRIPTPROPERTY(Name="isLoaded", Getter="IsLoaded")
    ZSCRIPTPROPERTY(Name="isValid", Getter="IsValid")

private:
    Level* m_Level {nullptr};
};

// =============================================================================
// SceneManager（Unity 的静态类）
// =============================================================================
//
// 全部是静态方法，所以（同 Application / Screen）导出成 `GetX()` 而不是
// `x` —— ZSCRIPTPROPERTY 的 getter 不允许 static，静态属性做不出来。

ZCLASS()
class SceneManager : public Object
{
    ZGENERATED_BODY(SceneManager)

public:
    /// 当前已加载的场景数。
    ZFUNCTION()
    static int GetSceneCount();

    /// 第 `index` 张已加载的场景。越界返回 nullptr（不抛，不夹紧 —— 夹紧会
    /// 让"我以为有 3 张其实只有 1 张"这个 bug 悄悄变成"又拿到第 0 张"）。
    ZFUNCTION()
    static Scene* GetSceneAt(int index);

    /// 当前活动场景。没有世界 / 没有活动关卡时返回 nullptr。
    ZFUNCTION()
    static Scene* GetActiveScene();

    /// 按**名字**找一张已加载的场景（用 `Scene.GetName()` 的同一套规则比对）。
    ZFUNCTION()
    static Scene* GetSceneByName(const eastl::string& name);

    /// 按路径 / URL 找一张已加载的场景。
    ZFUNCTION()
    static Scene* GetSceneByPath(const eastl::string& path);

    /// 这张 URL 是否已经加载。
    ZFUNCTION()
    static bool IsSceneLoaded(const eastl::string& path);

    /// 加载一张场景（Unity 的 `LoadScene`，单场景模式）：先卸掉所有已加载的，
    /// 再打开这一张，并把它设为活动场景。
    ///
    /// 与 Unity 的 `LoadScene` 一样**不是立即生效的语义保证**：这里同步完成加载，
    /// 所以调用返回后 `GetActiveScene()` 已经是新场景（Unity 要等到下一帧）。
    ZFUNCTION()
    static bool LoadScene(const eastl::string& path);

    /// 叠加加载（Unity 的 `LoadSceneAsync(..., LoadSceneMode.Additive)` 的同步版）。
    /// 不卸掉任何东西，也不改活动场景。
    ZFUNCTION()
    static bool LoadSceneAdditive(const eastl::string& path);

    /// 卸载一张已加载的场景。不能卸掉活动场景 —— 那会让引擎进入"没有活动关卡"
    /// 的状态，而 Level::Tick / GameObject.Find / Instantiate 全都依赖它。
    /// Unity 允许（它会回退到一张空场景），这里选择拒绝并返回 false。
    ZFUNCTION()
    static bool UnloadScene(const eastl::string& path);

    /// 重新加载当前活动场景（Unity 没有直接对应项，但这是 PIE 停止 / 重开一局
    /// 最常用的操作）。
    ZFUNCTION()
    static bool ReloadActiveScene();
};
