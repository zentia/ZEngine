# 运行时素材加载（`Resources`）

配套源码：

| 文件 | 作用 |
| --- | --- |
| `engine/Source/Runtime/Scripting/ScriptResources/ScriptResources.h/.cpp` | 脚本门面（Unity 的 `Resources`） |
| `engine/Source/Runtime/Resource/Asset/AssetManager.h/.cpp` | 真正读盘的一层（`GetAssetTypeName` / `ReadObject`） |
| `engine/Source/Runtime/File/FileSystem.h` | `.json` 素材的解码（`loadAssetJson`，本次修了一个返回值的 bug） |

## 0. 这次补的是什么

引擎有完整的素材管线，但**那全是 C++ 侧的**：组件在 `PostLoadResource` 里按路径取
素材，脚本什么都拿不到。于是"运行时动态取一个 prefab / 材质 / 配置"这件事在脚本
世界里根本做不到 —— 而这恰恰是 Unity 项目最常见的操作之一。

`ResourceManager::Load()` 看起来像是这个门面，但它是一个空壳：遍历一遍容器之后
`return nullptr`，全仓库没有任何调用方。本门面**不复用它**，直接落在
`AssetManager` 上。

```ts
Resources.Load(path): Object | null
Resources.LoadTyped(path, typeName): Object | null
Resources.Exists(path): boolean
Resources.GetResolvedPath(path): string      // 诊断：它到底去哪儿找了
Resources.GetTypeName(asset): string
Resources.UnloadAsset(asset): boolean
Resources.UnloadUnusedAssets(): number
Resources.GetLoadedCount(): number
```

## 1. 路径规则

Unity 的 `Resources.Load("Prefabs/Enemy")` 在**任意** `Assets/Resources/` 下找，
且不带扩展名。本引擎照搬这两条，另外接受绝对路径：

1. **绝对路径** —— 直接用（不存在时再逐个补扩展名试）；
2. **相对路径** —— 依次试 `<root>/Resources/<p>`、`<root>/<p>`，root 取
   素材目录 / 引擎根 / 引擎根下的 `Assets` / `asset` / 工程内容目录；
3. 上面都落空时，逐个补 `.zasset` `.mat` `.prefab` `.json` `.scene`
   `.timeline` 再试一遍。

`engine/asset/` 也在搜索根里 —— 只认 Unity 的 `Assets/` 拼写会让仓库里所有现成
素材都"找不到"。

绝对路径不套搜索根：`std::filesystem` 的 `operator/` 遇到绝对右值会整体替换，
套上去反而可能掩盖错误；明确地"直接用"更好诊断。

## 2. 类型只能由文件自己给

`Resources.Load` 返回 `Object`：素材的真实类型由**文件**声明，门面不猜。

* `.zasset` 的 ZASS 头里刻着类型名；
* `.mat` / `.prefab` / `.scene` / `.timeline` 是 YAML，用
  `ZYaml::PeekPrimaryObjectType` 从 fileID 1 上读出来；
* 读出来的名字在 `TypeManager` 里查不到 → 报错返回 `nullptr`（不是静默给一个裸
  `Object`）。

⚠️ **没有 `loadAsset<Object>()` 这条退路**，而且不该有：
`AssetManager::loadAsset<T>()` 按扩展名分派，`.json` 落到
`FileSystem::loadAssetJson<Object>()`，而后者要调 `Object::Transfer()` —— 那是
protected。所以 `loadAsset<Object>` 从来就没被实例化过（模板不实例化就不报这个
错）。退一步说，解出一个裸 `Object` 对脚本也毫无意义（没有任何成员）。

## 3. 缓存

Unity 的 `Resources.Load` 会缓存：同一路径两次调用返回**同一个实例**。本门面同样
缓存，键是**解析后的绝对路径**（Windows 上统一小写 —— NTFS 大小写不敏感，
`A/b.zasset` 和 `a/b.zasset` 是同一个文件，按调用方传进来的拼写缓存会读出两份
对象）。

⚠️ **已知盲区**：`AssetManager` 自己也有一张身份表
（`ObjectManager::IDToPointer(GetInstanceIDFromPathAndFileID(path, 1))`），它返回
的也是同一个指针。所以"第二次 `Load` 是被我的缓存接住的，还是被引擎的身份表接住
的"**从外部不可观测** —— 只有 eviction（`GetLoadedCount()` 的增减）是可见的。
`.mutate_re.py` 里因此没有"去掉缓存查找"这一条变异（抓不到），只有 emplace /
erase 相关的三条。

## 4. `UnloadAsset` / `UnloadUnusedAssets` 不释放内存

Unity 的这两个 API 会真的释放（引擎知道没人在用）。本引擎的素材由
`ObjectManager` 持有、没有引用计数，门面无从判断"没人在用"，贸然 `delete` 会把
还在被引用的对象变成悬垂指针。

所以这里的语义是确定的那半边：

* `UnloadAsset(asset)` —— 从缓存里摘掉，下次 `Load` 会重新走一遍加载路径；
* `UnloadUnusedAssets()` —— 清空缓存，返回被摘掉的条数。

## 5. 顺手修的一个真 bug：`FileSystem::loadAssetJson`

```cpp
template<typename AssetType>
AssetType* loadAssetJson(const std::filesystem::path& asset_path) const
{
    ...
    return true;    // ← 返回类型是 AssetType*，这就是 (AssetType*)1
}
```

一个**非空但完全无效**的指针。调用方（`AssetManager::loadAsset<T>()`）只判空，
于是"加载成功"、一解引用就崩，而且刚 `CreateObject` 出来的那个对象还泄漏了。
错误分支的 `return false` 恰好被解释成 `(AssetType*)0`，看起来反而"对"，所以这个
bug 一直没暴露。

模板不实例化就不会被发现：这次是因为 `Resources.Load` 第一次尝试用
`loadAsset<Object>()` 才让它现形（随后报的是 `Object::Transfer` is protected，
见 §2）。

## 6. 测试与变异覆盖

`UnityParitySmokeTest` 的 **AX 组（行为，27 条）** 与 **AY 组（接线门，10 条）**。

⚠️ **为什么用引擎现写一张 `.scene` 来测，而不是读仓库里的 `.zasset`**：那些是
cooked 产物，读它要 `SerializedFile` + `FileCacher` + `MemoryManager` 全套初始化，
无头下拿不到 —— 第一次试就是直接段错误。改用 `AssetManager::WriteObjectsToYaml()`
现写一张 `.scene`：写出与读回走的是同一套 YAML 图模型，`Resources.Load` 走的加载
路径是真的（类型名从文件里读、`ObjectManager` 按类型 `Produce`、按图反序列化），
只是素材来源换成了这一张。

路径解析那一半用**绝对路径**驱动：相对路径要 `ConfigManager` / `ProjectInfo` 提供
搜索根，而那两个的初始化依赖 `CommandSystem` 与磁盘上的 ini，无头下不可控。

`.mutate_re.py` 有 12 个变异，全部被抓到（盲区见 §3）。
