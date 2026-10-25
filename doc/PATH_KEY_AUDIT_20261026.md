# 路径作查找键的同类问题扫描（2026-10-26）

起因：`5eb35b1d` 修了"同一文件的相对 / 绝对写法被当成两个文件"（路径→序列化文件索引按原字符串查）。
本文把全树里**拿路径字符串当 map 键或比较对象**的地方扫一遍，逐个判定。

扫描方法：`grep -rnE "(unordered_)?map<\s*(std::)?(string|filesystem::path|eastl::string)\s*,"` 限 Runtime / Editor，
再筛键语义是路径 / URL / GUID 的，逐个读写入点和查询点。

## 判定结果

| 位置 | 键 | 写入 / 查询的规范化 | 判定 | 处理 |
|---|---|---|---|---|
| `RuntimeAssetManager` / `EditorAssetManager` 路径→文件索引 | 路径 | 写 `CanonicalPathKey`（5eb35b1d） | 已修；**缺大小写折叠** | 本次补：Windows 上小写（与 `DeterministicGuidFromPath` 同约定） |
| `DeterministicGuidFromPath`（YAML 资产 / 无显式 GUID 的写入） | 路径→GUID | 只 `lexically_normal`，**相对路径不锚定** | **真问题**：文档约定输入是绝对路径，但存储的路径名保留首次写法（可能是相对的），同一文件可派生两个 GUID | 本次修：相对路径按当前目录词法锚定 |
| `EditorAssetManager::GetAssetGuidAndType` | 注册表相对键 | 绝对路径转成相对扫描根；**相对路径原样当键** | **真问题**：相对写法是相对当前目录的，不是相对扫描根，查不到注册表就落到路径哈希 | 本次修：先锚定到当前目录再转相对扫描根；转出来以 `..` 开头（不在扫描根下）时不用 |
| `AssetBundle::FindEntry` | 注册表相对键 | 精确查 + 路径边界后缀匹配 | **部分**：`./x`、绝对路径已被后缀匹配覆盖；`a/b/../x`、反斜杠写法查不到 | 本次修：查询前折叠反斜杠 + `lexically_normal` |
| `TextureImportSettingsRegistry` | 绝对路径 | `generic_string` + Windows 小写，**无 lexically_normal** | 轻微：`./`、`..` 写法会错过 | 本次修：加 `lexically_normal`（对已规范的绝对路径是恒等，磁盘上已存的键仍匹配） |
| `SourceAssetRegistry` | 绝对路径 | `weakly_canonical` + 小写 | 正确 | — |
| `ScriptRegistry` / `ShaderRegistry` | 相对路径 | generic + 小写 / `NormaliseRelPath` | 正确 | — |
| `AssetRegistry::GetAssetIndex` | 注册表相对键 | **调用方负责**；目录遍历产出的键保留磁盘大小写 | 依赖调用方；已知唯一不规范的调用方是上面 `GetAssetGuidAndType` | 修调用方 |
| `WorldManager::m_LoadedLevels` / `m_LevelUrlRefCounts` | 关卡 URL | 原样 | **观察到，未改**：同一关卡用两种写法加载会加载两份。URL 是 API 层的身份约定，调用方（世界分区、编辑器）都用注册表 / 资源浏览器给的同一写法，改它会影响存档 / 分区数据里存的 URL | 记录，不改 |
| Spine / 字体 / Profiler 等缓存 | 路径或名字 | 原样 | 只是缓存，两种写法 = 多缓存一份，不影响正确性 | — |

## 验证

- `PrefabEndToEndSmokeTest` P9 加：相对 / 绝对 / `./` 写法派生同一个 GUID；Windows 上大小写不同写法同一实例 ID（`#if _WIN32`，
  **web 无头环境不走这个分支，只有 clang-cl 类型检查**）。
- `AssetBundleSmokeTest` B5 加 `./`、`..`、反斜杠三种写法。
- 变异：GUID 不锚定 ⇒ P9 两项红；bundle 查询不做 `lexically_normal` ⇒ `..` 那项红（`./` 和反斜杠分别被原有后缀匹配、
  反斜杠折叠兜住，这两项不能单独证明 `lexically_normal` 的作用）。
- `EditorAssetManager::GetAssetGuidAndType`、`TextureImportSettingsRegistry` 的改动需要编辑器的 AssetRegistry，
  无头环境起不来，**只有类型检查**。

## 行为变化提示

`DeterministicGuidFromPath` 对**相对路径**的输出变了（锚定后哈希的是绝对路径）。用相对路径写出过、且没显式给 GUID 的
`.zasset`，下次重写时 GUID 会变成与 AssetRegistry 扫描一致的值。外部引用读取时先按路径解析、GUID 只在路径失效时兜底，
所以已有场景的引用不受影响；AssetRegistry 缓存里的 YAML 资产 GUID 会在下次扫描刷新。
