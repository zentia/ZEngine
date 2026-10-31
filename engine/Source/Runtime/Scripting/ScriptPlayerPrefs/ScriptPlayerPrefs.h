#pragma once

#include "Runtime/BaseClasses/Object.h"

#include "ScriptPlayerPrefs.generated.h"

class UserPreferences;

// Unity 的 `PlayerPrefs`：跨会话持久的键值存档。
//
// 它底下就是 `UserPreferences`（Runtime/Resource/UserPreferences）—— 一个
// IEngineSystem，而不是 ZCLASS，所以脚本此前**完全碰不到**：想存一个"最高分"只能
// 自己去写文件，于是每个工程都会各写一份、路径各不相同。
//
// Unity 的 `PlayerPrefs` 是**静态类**。这里的 ZSCRIPTPROPERTY 不支持静态访问器
// （`zht/parser.py` 会直接报错），所以落成 `ZCLASS + 静态方法`，脚本写
// `PlayerPrefs.SetInt(...)` 而不是 `PlayerPrefs.int = ...`。与 `Time` / `Screen` /
// `Camera` 同一处取舍（ZCLASS 必须派生 Object，纯静态类进不了绑定表）。
//
// 存什么、不存什么
// ----------------
// 这是**明文 JSON**，放在用户目录下任何进程都能读改。存档、进度、设置适合；密码、
// 令牌、任何"改了就是作弊"的数值不适合 —— 那些要自己加密或者走服务端。
//
// 落盘时机
// --------
// 写操作只改内存并标脏，真正写盘在 `Save()` 和引擎退出（`Shutdown`）时。所以
// 一次崩溃会丢掉最后一次 Save 之后的写入 —— 想"立刻生效"就显式调 `Save()`，
// 但别在每帧调（那是每次一次全量文件重写）。
ZCLASS()
class PlayerPrefs : public Object
{
    ZGENERATED_BODY(PlayerPrefs)

public:
    // ---- 写入 ----------------------------------------------------------

    ZFUNCTION()
    static void SetInt(const eastl::string& key, int value);

    ZFUNCTION()
    static void SetFloat(const eastl::string& key, float value);

    ZFUNCTION()
    static void SetString(const eastl::string& key, const eastl::string& value);

    /// Unity 没有这两个（那边是 `SetInt(key, value ? 1 : 0)`），但每个 Unity 工程
    /// 都会自己写一遍这两个扩展方法，所以这里直接给。存的是真正的 JSON bool，
    /// 不是 0/1 —— 于是 `GetBool` 读一个 `SetInt(k, 1)` 写下的值也能正确返回 true。
    ZFUNCTION()
    static void SetBool(const eastl::string& key, bool value);

    // ---- 读取 ----------------------------------------------------------

    /// 键不存在时返回 `default_value`（Unity 同）。
    ///
    /// **默认值参数必须传**：不传的话"没存过"和"存了个 0"在脚本里是一样的，
    /// 于是"第一次进游戏"这个分支永远走不到。
    ZFUNCTION()
    static int GetInt(const eastl::string& key, int default_value = 0);

    ZFUNCTION()
    static float GetFloat(const eastl::string& key, float default_value = 0.0f);

    ZFUNCTION()
    static eastl::string GetString(const eastl::string& key, const eastl::string& default_value = eastl::string());

    ZFUNCTION()
    static bool GetBool(const eastl::string& key, bool default_value = false);

    // ---- 管理 ----------------------------------------------------------

    ZFUNCTION()
    static bool HasKey(const eastl::string& key);

    ZFUNCTION()
    static void DeleteKey(const eastl::string& key);

    /// 清空**全部**偏好。没有撤销。清完不自动落盘（见 UserPreferences::DeleteAll）。
    ZFUNCTION()
    static void DeleteAll();

    /// 立刻把内存里的改动写进磁盘。正常退出时引擎自己会调一次，所以只在"必须
    /// 马上生效"时手动调。
    ZFUNCTION()
    static void Save();

private:
    /// 当前的用户偏好系统；没有注册时为 nullptr，此时所有读返回默认值、所有写是
    /// 空操作（**不**静默失败到崩溃）。
    static UserPreferences* ResolvePreferences();
};
