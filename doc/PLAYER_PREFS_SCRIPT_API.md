# `PlayerPrefs` 脚本 API（Unity 同名静态类）

切片 12。`UserPreferences`（`Runtime/Resource/UserPreferences`）早就实现完整了 ——
int / float / string / bool、JSON 落盘、跨会话持久 —— 但它是 `IEngineSystem` 而不是
`ZCLASS`，所以脚本**一个字都碰不到**：想存一个"最高分"只能自己写文件，于是每个工程
各写一份、路径各不相同。

这个切片给它套一层 Unity 拼写的脚本门面。

## API

```ts
PlayerPrefs.SetInt(key, value);      PlayerPrefs.GetInt(key, defaultValue = 0);
PlayerPrefs.SetFloat(key, value);    PlayerPrefs.GetFloat(key, defaultValue = 0);
PlayerPrefs.SetString(key, value);   PlayerPrefs.GetString(key, defaultValue = "");
PlayerPrefs.SetBool(key, value);     PlayerPrefs.GetBool(key, defaultValue = false);

PlayerPrefs.HasKey(key);
PlayerPrefs.DeleteKey(key);
PlayerPrefs.DeleteAll();
PlayerPrefs.Save();
```

`SetBool` / `GetBool` 是 Unity 没有的两个（那边写 `SetInt(key, value ? 1 : 0)`），
但每个 Unity 工程都会自己补一遍，所以这里直接给。存的是真正的 JSON bool，于是
`GetBool` 读一个 `SetInt(k, 1)` 写下的值也能正确返回 `true`。

## 默认值参数是这个 API 的一半意义

`GetInt(key, defaultValue)` 的 `defaultValue` **必须传**。少了它，"没存过"和"存了个 0"
在脚本里是一样的，于是"第一次进游戏"这个分支永远走不到 —— 而那正是 PlayerPrefs
最经典的用法：

```ts
const best = PlayerPrefs.GetInt("bestScore", 0);
if (PlayerPrefs.HasKey("bestScore")) { /* 老玩家 */ } else { /* 第一次 */ }
```

`HasKey` 与 `GetInt(key, 0) == 0` 是**两个不同的问题**：存了个 0 的键是存在的。
`HasKey` 回答前者。

## 明文存储

这是**明文 JSON**，落在用户目录下，任何进程都能读改。

* 适合：设置、最高分、进度、音量、上次选的角色。
* **不适合**：密码、令牌、任何"改了就是作弊"的数值。那些要自己加密或走服务端。

这条警告也写在 `ScriptPlayerPrefs.h` 的头部，接线门（AF7）守着它不被删掉。

## 落盘时机

写操作只改内存并标脏；真正写盘在 `Save()` 和引擎退出（`Shutdown`）时。

所以一次崩溃会丢掉最后一次 `Save()` 之后的写入。想"立刻生效"就显式调 `Save()`，
但**别每帧调** —— 那是一次全量文件重写。

`DeleteAll()` 清完**不**自动落盘：它通常紧跟一次全量重写，中间那次写盘是白写的。

## 为什么是 `PlayerPrefs.SetInt()` 而不是 `PlayerPrefs.int`

Unity 的 `PlayerPrefs` 是静态类。这里的 `ZSCRIPTPROPERTY` 不支持静态访问器
（`zht/parser.py` 直接报错），所以落成 `ZCLASS + 静态方法`。与 `Time` / `Screen` /
`Camera` / `Physics` 同一处取舍（ZCLASS 必须派生 Object，纯静态类进不了绑定表）。

## 降级契约

没有注册 `UserPreferences` 时（headless、编辑器未初始化、平台不支持）：

* 所有 **读** 返回调用方给的默认值
* 所有 **写** 是空操作（不是崩，也不是静默失败到崩溃）
* `HasKey` 返回 **false** —— "没有任何键"是唯一诚实的答案；说 true 会让脚本以为
  读得到

每一个入口都先 `ResolvePreferences()` 再判空（AF9 数了 13 处，一处都不能少）。

## 测试

`UnityParitySmokeTest` 的 **AE 组**（26 项）与 **AF 组**（14 项，接线门）。

AE 组先测"没有偏好系统"的降级分支，**再**注册一台**不调 `Initialize()`** 的
`UserPreferences` 测真实读写：

> `Initialize()` 需要 `GET_SYSTEM(PlayerSettings)`，而且会真的在用户目录下建目录、
> 读文件。不 Initialize 的话 `m_PreferencesFilePath` 是空的，所有读写都在内存里跑
> —— 行为完全一致，且不碰磁盘。这同时顺带验证了 `save()` 的空路径守卫。

顺带补的两个实现缺口：

* `UserPreferences::DeleteAll()` —— 此前不存在（Unity 的 `PlayerPrefs.DeleteAll`）。
* `UserPreferences::save()` 的空路径守卫 —— 路径为空时 `create_directories("")` 会抛
  异常、被 catch 吞掉，是一个**静默**的失败。现在显式早退。

9 个变异全部验证为红。

总计 852 checks / 0 failures。
