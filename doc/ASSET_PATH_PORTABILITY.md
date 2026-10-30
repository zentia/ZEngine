# 资产路径可移植性

> 门禁：`AssetPathPortabilitySmokeTest`（`-DZENGINE_BUILD_ASSET_PATH_PORTABILITY_SMOKE_TEST=ON`）

## 1. 问题

2026-10-30 之前，9 个文本资产里嵌着**作者机器的绝对路径**：

```yaml
# engine/asset/objects/environment/floor/floor.prefab
externals:
  - guid: 4fe18f6deeddf4258b310168830a31e0
    type: Box
    path: "E:/Engine/ZEngine/engine/asset/objects/environment/floor/geometry/floor.object_shape0.zasset"
```

后果不是崩溃，是**换机器/换盘静默断链**。`AssetManager::ResolveExternalAssetPath`
拿到绝对路径时先按原样 `exists()` 试一次，试不到才退回"工程根 + 相对路径"。于是：

* 作者机器：绝对路径存在 ⇒ 一切正常；
* 其它机器：`exists()` 失败 ⇒ 退回相对解析 ⇒ 这串里没有相对部分可用 ⇒ PPtr 变 null。

表现是"角色没有控制器参数、墙没有碰撞体"，而**没有任何一条日志**。

## 2. 正确形式

工程根 = `ZGameSmoke.zproject` 所在目录（`ProjectInfo::project_path =
project_file.parent_path()`），也就是 `<repo>/engine`。所以相对形式是：

```
asset/objects/environment/floor/geometry/floor.object_shape0.zasset
```

与同文件内其它路径（`skeleton_file_path: asset/objects/...`）一致。

`AssetManager::StoreExternalAssetPath()` 本来就会把绝对路径转成这种工程相对形
式 —— 这几个文件是没走这个函数的陈旧产物。

## 3. 修了什么

| 文件 | 处数 |
| --- | --- |
| `objects/environment/stairs/stairs.prefab` | 8 |
| `objects/environment/wall/wall_with_window.prefab` | 7 |
| `objects/environment/fence/fence.prefab` | 4 |
| `objects/environment/wall/wall_with_door.prefab` | 3 |
| `objects/character/player/player.prefab` | 2 |
| `objects/environment/floor/floor.prefab` | 1 |
| `objects/environment/wall/wall.prefab` | 1 |
| `objects/environment/wall/wall_block.prefab` | 1 |
| `.../animation/data/W2_CrouchWalk_Aim_F_Loop_IP.animation_clip.json` | 1 |

最后一行是门禁自己找出来的（我最初只 grep 了 `E:/Engine/ZEngine`，漏了这条来自
另一台机器的 `D:/data/workspace/zentiatest/zentia/...`）。它的
`skeleton_file_path` 指向一个本工程根本不存在的骨架；按骨骼名比对
（`biped` / `biped Head` / `biped L Calf` … 与 `skeleton_data_root.skeleton.json`
的 58 根完全吻合）改成了工程自己的骨架。

## 4. 门禁的三层断言

| 组 | 钉什么 | 为什么必须有 |
| --- | --- | --- |
| A | 扫描覆盖率（文本资产 ≥10、prefab ≥5） | 根路径写错 ⇒ 0 个文件 ⇒ B/C 全绿。最典型的空测试 |
| B | 无 Windows 盘符 / POSIX home 绝对路径 | 主体 |
| C | externals 里的相对路径**真能解析到文件** | B 抓不到"路径是相对的但相对错了" —— 同样断链、同样静默 |

B 与 C 互补，已用两个变异分别验证：

* 回灌一条绝对路径 ⇒ 只有 **B1** 红；
* 改成相对但拼错文件名 ⇒ 只有 **C2** 红（B 组全绿）。

## 5. 顺带修掉的一个空测试

`AnimationPipelineSmokeTest.cpp` 的 P8 组要读仓库里一份真实历史资产来验证
`SafeBinaryRead` 容忍缺失字段。那条路径也硬编码成 `E:/Engine/ZEngine/...`，外面
还包着 `if (std::filesystem::exists(...))` ⇒ **除了作者机器，P8 全程静默跳过**，
而测试照样全绿。

已改成从 `ZENGINE_ENGINE_ROOT_DIR` 推导，并且文件不存在时**判失败**（它在仓库
里，找不到就是真问题）。

> 这类"包在 `if (exists)` 里的可选断言"是空测试的第二大来源（第一大是扫描范围
> 为空）。新增可选断言时，先问一句：它在别人的机器上会不会静默变成零条。

## 6. 遗留

| 项 | 说明 |
| --- | --- |
| 派生 `.zasset` 里的陈旧绝对路径 | 门禁**不扫** `.zasset`：它是导入器从同名 `.json` 再生的派生产物，权威源在 `.json`，守源即可。当前 `W2_CrouchWalk_Aim_F_Loop_IP.animation_clip.zasset` 里仍有一份 `D:/data/...` 的旧值 —— 运行时按扩展名分发、`loadAsset` 读的是 `.json`，所以这条**当前是惰性的**；重新导入一次即可清掉 |
| `_Generated/` | 已被 `.gitignore` 排除，是构建产物，不管 |
| 通用"重新导入"CLI | `-batchmode` 目前只跑工具套件（`-suite=`），跑不了 `asset.reimport`。要脚本化重烘焙得先补一个入口 |
