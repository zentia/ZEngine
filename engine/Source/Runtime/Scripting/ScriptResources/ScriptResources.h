#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "ScriptResources.generated.h"

// =============================================================================
// `Resources` —— Unity 的 `Resources.Load` 那套运行时素材加载。
//
// 为什么需要它
// ------------
// 引擎有完整的素材管线（`AssetManager::loadAsset<T>()` / `ReadObject()` /
// AssetBundle），但**那全是 C++ 侧的**：组件在 `PostLoadResource` 里按路径取
// 素材，脚本什么也拿不到。于是脚本世界里"运行时动态取一个 prefab / 材质 / 配置"
// 这件事根本做不到 —— 而这恰恰是 Unity 项目最常见的操作之一。
//
// `ResourceManager::Load()` 看起来像是这个门面，但它是一个空壳（遍历一遍容器
// 之后 `return nullptr`），没有任何调用方。本门面不复用它，直接落在
// `AssetManager` 上：那才是真正会去读盘的一层。
//
// 路径规则
// --------
// Unity 的 `Resources.Load("Prefabs/Enemy")` 在**任意** `Assets/Resources/` 下
// 找，且不带扩展名。本引擎照搬这两条，另外接受绝对路径（打包后、以及任何"我
// 手里已经有完整路径"的场合都要能用）：
//
//   1. 绝对路径 —— 直接用（不存在时再逐个补扩展名试）；
//   2. 相对路径 —— 依次试 `<root>/Resources/<p>`、`<root>/<p>`，root 取
//      素材目录 / 引擎根 / 引擎根下的 Assets / asset / 工程内容目录；
//   3. 上面都没有时，逐个补 `.zasset` `.mat` `.prefab` `.json` `.scene`
//      `.timeline` 再试一遍（不带扩展名是 Unity 的默认写法）。
//
// 缓存
// ----
// Unity 的 `Resources.Load` 会缓存：同一个路径两次调用返回**同一个实例**，所以
// 脚本可以放心每帧 `Load` 然后比较引用。本门面同样按**解析后的绝对路径**缓存
// （不是按调用方传进来的拼写 —— `a/b` 与 `A/b` 在 Windows 上是同一个文件，
// 按原拼写缓存会拿到两份）。
// =============================================================================

/// 运行时素材加载（Unity 的 `Resources` 静态类）。
///
/// 与 `SceneManager` / `Application` 一样全部是静态方法，所以导出成 `Load()`
/// 而不是 `load` —— `ZSCRIPTPROPERTY` 的 getter 不允许 static，静态属性做不出来。
ZCLASS()
class Resources : public Object
{
    ZGENERATED_BODY(Resources)

public:
    /// 加载 `path` 指向的素材，返回缓存里的实例（首次调用才真的读盘）。
    ///
    /// 找不到 / 读不出来返回 `nullptr` 并打日志 —— **不要**静默返回空：一个
    /// "从来没配过路径"和"配错了路径"在日志里必须能分开。
    ///
    /// 返回的静态类型是 `Object`：素材的真实类型由文件自己决定，脚本要按类型
    /// 断言就用 `LoadTyped` 或 `GetTypeName`。
    ZFUNCTION()
    static Object* Load(const eastl::string& path);

    /// 按**指定类型**加载。类型对不上（或类型名不认识）返回 `nullptr`。
    ///
    /// 这是 Unity 的 `Resources.Load<T>(path)` 在本引擎的形状：TS 的泛型在编译
    /// 后就被擦掉了，门面拿不到 `T`，所以显式传类型名。比"先 Load 再自己判
    /// 类型"强的地方在于**读盘时就按这个类型解**，类型不对不会先把对象造出来。
    ZFUNCTION()
    static Object* LoadTyped(const eastl::string& path, const eastl::string& type_name);

    /// `path` 是否解析得到一个真实存在的文件。只查文件系统，不加载。
    ZFUNCTION()
    static bool Exists(const eastl::string& path);

    /// `path` 解析之后的**绝对**路径；解析不到返回空串。
    ///
    /// 诊断用：素材加载失败时第一步永远是"它到底去哪儿找了"。
    ZFUNCTION()
    static eastl::string GetResolvedPath(const eastl::string& path);

    /// 这个素材的运行时类型名（`Object::GetTypeName()`）；传 `nullptr` 返回空串。
    ZFUNCTION()
    static eastl::string GetTypeName(Object* asset);

    /// 把这条素材从缓存里摘掉。返回它原来是否在缓存里。
    ///
    /// ⚠️ **不会销毁对象**。Unity 的 `UnloadAsset` 会真的释放（它知道没人在用），
    /// 本引擎的素材由 `ObjectManager` 持有、没有引用计数，门面无从判断"没人在
    /// 用"，贸然 `delete` 会把还在被引用的对象变成悬垂指针。摘出缓存的语义是
    /// "下次 `Load` 会重新读盘"，这一点是确定的。
    ZFUNCTION()
    static bool UnloadAsset(Object* asset);

    /// 清空整个缓存，返回被摘掉的条数。
    ///
    /// 同上：**不释放内存**。Unity 的 `UnloadUnusedAssets` 会真的回收没人引用的
    /// 素材（并返回释放的字节数语义上的计数）；这里只能诚实地交出"缓存里有多
    /// 少条被丢掉了"。
    ZFUNCTION()
    static int UnloadUnusedAssets();

    /// 当前缓存里有多少条素材。
    ZFUNCTION()
    static int GetLoadedCount();
};
