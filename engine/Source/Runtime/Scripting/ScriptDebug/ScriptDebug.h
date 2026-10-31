#pragma once

// =============================================================================
// `Debug` 脚本对象的 C++ 侧。
//
// 为什么**不是** `ZCLASS() class Debug : public Object`
// ------------------------------------------------------
// 脚本侧的 `Debug` 已经有了一份 `declare namespace Debug`（ScriptBinding.cpp
// 的 .d.ts 模板）和一个纯 JS 对象（ScriptEnv 的 shim）。把它换成绑定类会带来
// 两处回归：
//
//   1. ZFUNCTION 的签名是固定的 `Log(message: string)`，而现在的 shim 支持
//      `Debug.Log("a", 1, obj)` 任意个参数（JS 侧 String() 之后拼接）。Unity
//      的 `Debug.Log(object message)` 在 TS 里也常被写成多参。
//   2. 绑定类的全局是**构造器**，安装时会把 JS 对象整体覆盖掉。
//
// 所以这里只放"JS 做不了、必须回到 C++"的那几个：`IsDebugBuild()`（编译期
// 事实）与 `Break()`（要碰调试器）。其余（Assert / LogException）留在 shim
// 里用 JS 拼装，或者干脆不动。
//
// 命名成 `ScriptDebug` 命名空间而不是 `class Debug`，就是为了不和脚本侧的
// `Debug` 抢名字 —— ZHeaderTool 见到 `ZCLASS` 才会发布同名类，这里没有
// ZCLASS，也就不存在冲突。
// =============================================================================

namespace ScriptDebug
{
    /// 当前二进制是不是"开发构建"。Unity 的 `Debug.isDebugBuild`。
    ///
    /// 判据是 `NDEBUG`：MSVC 的 Debug / RelWithDebInfo 不定义它，Release 定义。
    /// 与"有没有调试符号"不是一回事（RelWithDebInfo 有符号但这里返回 true），
    /// 这正是 Unity 的语义 —— 它是"开发版还是发布版"，不是"能不能调"。
    bool IsDebugBuild();

    /// Unity 的 `Debug.Break()`：日志留痕 + 挂住调试器。
    ///
    /// ⚠️ **没有调试器时绝不真的中断**。Unity 里 Debug.Break 是"暂停播放器"，
    /// 而在这里如果无条件 `__debugbreak()`，CI 上任何一条脚本调用都会让测试
    /// 进程以 0x80000003 直接死掉 —— 连日志都来不及刷。所以先 `IsDebuggerPresent()`
    /// （Windows）/ 其它平台同理，没有调试器就只记一条 error。
    void Break(const char* message);
}  // namespace ScriptDebug
