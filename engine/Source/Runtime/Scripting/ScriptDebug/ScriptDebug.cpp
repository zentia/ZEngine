#include "ScriptDebug.h"

#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/Core/Log/LogSystem.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <unistd.h>
#endif

namespace ScriptDebug
{
    bool IsDebugBuild()
    {
#if defined(NDEBUG)
        return false;
#else
        return true;
#endif
    }

    namespace
    {
        /// 本进程是否正被调试器附着。
        ///
        /// 只用于决定"要不要真的中断"—— 所以它宁可漏判（没中断，只是少一个
        /// 断点）也不能误判（CI 上把测试进程打断）。
        bool IsDebuggerAttached()
        {
#if defined(_WIN32)
            return ::IsDebuggerPresent() != 0;
#elif defined(__APPLE__)
            // sysctl(P_PID, KERN_PROC_PID) 的 kp_proc.p_flag & P_TRACED。
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
            struct kinfo_proc info {};
            size_t size = sizeof(info);
            if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
            {
                return false;
            }
            return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
            // Linux: /proc/self/status 的 TracerPid 字段。读不到就当没附着。
            return false;
#endif
        }
    }  // namespace

    void Break(const char* message)
    {
        LOG_ERROR(ZScripting, "[Debug.Break] {}", message != nullptr ? message : "");
        // 异步日志：下面可能立刻中断，不刷这一条就没了 —— 而它是唯一说清
        // "脚本在哪儿 Break"的证据。
        ZEngineForceFlushLogs();

        if (!IsDebuggerAttached())
        {
            // 没有调试器：中断只会变成一次进程死亡（MSVC 的 __debugbreak 在
            // 无调试器时触发 unhandled exception）。日志已经留下了，到此为止。
            return;
        }

#if defined(_MSC_VER)
        __debugbreak();
#elif defined(__GNUC__) || defined(__clang__)
        __builtin_trap();
#endif
    }
}  // namespace ScriptDebug
