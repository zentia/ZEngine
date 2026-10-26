// =============================================================================
// ZEngine Android Player — NativeActivity 入口
// -----------------------------------------------------------------------------
// 用 NDK 自带的 android_native_app_glue + NativeActivity，Java 侧零代码：
// AndroidManifest 里把 Activity 声明成 android.app.NativeActivity，系统加载
// libZGameAndroid.so 并调用 android_main()。
//
// 为什么不用 GameActivity（AGDK）：那需要把 GameActivity / GameController
// 两个库放进 3rdparty 并维护 AGP 依赖；native_app_glue 是 NDK 自带的，
// 触摸事件同样走 AInputEvent，功能上够 P0 用。
//
// 这里只做三件事，业务逻辑一律在 Player 壳里：
//   1. 把 APK assets 里的项目内容解到内部可写目录（引擎的 Android 文件系统
//      是裸 POSIX，打不开 APK 内的只读资源）
//   2. 把 ANativeWindow 的生命周期映射成 ZPlayer_SetNativeWindow / Resize /
//      Pause / Resume
//   3. 每帧调 ZPlayer_Tick
// =============================================================================

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Player/PlayerShell.h"

namespace
{
    constexpr const char* k_log_tag = "ZEngine";

    /// APK assets 下放项目内容的子目录（由 Gradle 的 assets.srcDirs 打进去）。
    constexpr const char* k_assets_root = "zengine";

    /// 解包目标目录名（位于 app 的内部文件目录下）。
    constexpr const char* k_project_dir_name = "zengine_project";

    struct AssetEntry
    {
        std::string rel_path;  // 相对 k_assets_root 的路径
        uint64_t    size {0};
        bool        is_dir {false};
    };

    bool        g_window_ready {false};
    bool        g_destroy_requested {false};
    std::string g_project_root;
    std::string g_files_dir;

    void AndroidLog(int priority, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    void AndroidLog(int priority, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        __android_log_vprint(priority, k_log_tag, fmt, args);
        va_end(args);
    }

    /// 目录还是文件？AAssetManager 没有直接的类型查询：openDir 对目录返回非
    /// null（空目录也是），对文件返回 null。
    bool AssetIsDirectory(AAssetManager* mgr, const std::string& rel)
    {
        AAssetDir* dir = AAssetManager_openDir(mgr, rel.c_str());
        if (dir == nullptr)
        {
            return false;
        }
        AAssetDir_close(dir);
        return true;
    }

    bool CollectAssetEntries(AAssetManager* mgr,
                             const std::string& rel_dir,
                             const std::string& rel_root,
                             std::vector<AssetEntry>& out)
    {
        AAssetDir* dir = AAssetManager_openDir(mgr, rel_dir.c_str());
        if (dir == nullptr)
        {
            return false;
        }

        const char* name = nullptr;
        while ((name = AAssetDir_getNextFileName(dir)) != nullptr)
        {
            const std::string child = rel_dir.empty() ? name : rel_dir + "/" + name;

            if (AssetIsDirectory(mgr, child))
            {
                out.push_back({child, 0, true});
                if (!CollectAssetEntries(mgr, child, rel_root, out))
                {
                    AAssetDir_close(dir);
                    return false;
                }
                continue;
            }

            off_t size = 0;
            if (AAsset* asset = AAssetManager_open(mgr, child.c_str(), AASSET_MODE_UNKNOWN); asset != nullptr)
            {
                size = AAsset_getLength(asset);
                AAsset_close(asset);
            }
            out.push_back({child, static_cast<uint64_t>(size), false});
        }

        AAssetDir_close(dir);
        return true;
    }

    /// 内容指纹：文件名 + 大小过一遍 FNV-1a。用来判断 APK 里的 assets 有没有变，
    /// 变了才重新解包 —— 否则每次启动都要拷一遍整个项目。
    uint64_t Fingerprint(const std::vector<AssetEntry>& entries)
    {
        uint64_t hash = 1469598103934665603ull;  // FNV offset basis
        for (const AssetEntry& e : entries)
        {
            for (const char c : e.rel_path)
            {
                hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
                hash *= 1099511628211ull;
            }
            hash ^= e.size;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::filesystem::path StampPath(const std::filesystem::path& project_root)
    {
        return project_root / ".zengine_assets_stamp";
    }

    uint64_t ReadStamp(const std::filesystem::path& project_root)
    {
        std::ifstream in(StampPath(project_root), std::ios::binary);
        if (!in)
        {
            return 0;  // 0 = 从未解包过
        }
        uint64_t value = 0;
        in >> value;
        return value;
    }

    void WriteStamp(const std::filesystem::path& project_root, uint64_t stamp)
    {
        std::error_code ec;
        std::filesystem::create_directories(project_root, ec);
        std::ofstream out(StampPath(project_root), std::ios::binary | std::ios::trunc);
        out << stamp;
    }

    bool CopyOneAsset(AAssetManager* mgr, const std::string& asset_path, const std::filesystem::path& dest)
    {
        AAsset* asset = AAssetManager_open(mgr, asset_path.c_str(), AASSET_MODE_STREAMING);
        if (asset == nullptr)
        {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);

        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            AAsset_close(asset);
            return false;
        }

        char buffer[64 * 1024];
        int  read_bytes = 0;
        while ((read_bytes = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
        {
            out.write(buffer, read_bytes);
        }

        AAsset_close(asset);
        return true;
    }

    /// 把 assets/<k_assets_root> 整棵解到 project_root。
    /// 返回解出的文件数；assets 根本不存在时返回 -1（调用方据此退回"内容已经
    /// 在设备上"的调试形态）。
    long long ExtractPackagedAssets(AAssetManager* mgr, const std::filesystem::path& project_root)
    {
        std::vector<AssetEntry> entries;
        if (!CollectAssetEntries(mgr, k_assets_root, k_assets_root, entries) || entries.empty())
        {
            return -1;
        }

        const uint64_t stamp = Fingerprint(entries);
        if (stamp != 0 && stamp == ReadStamp(project_root))
        {
            AndroidLog(ANDROID_LOG_INFO, "assets unchanged (stamp %llu), skipping extract",
                       static_cast<unsigned long long>(stamp));
            return 0;
        }

        long long copied = 0;
        for (const AssetEntry& e : entries)
        {
            // 条目里的路径是相对 APK assets 根的（带 "zengine/" 前缀），落到磁盘
            // 时要剥掉，让 project_root 直接就是项目根。
            std::string rel = e.rel_path;
            const std::string prefix = std::string(k_assets_root) + "/";
            if (rel.rfind(prefix, 0) == 0)
            {
                rel = rel.substr(prefix.size());
            }

            const std::filesystem::path dest = project_root / rel;
            if (e.is_dir)
            {
                std::error_code ec;
                std::filesystem::create_directories(dest, ec);
                continue;
            }

            if (CopyOneAsset(mgr, e.rel_path, dest))
            {
                ++copied;
            }
            else
            {
                AndroidLog(ANDROID_LOG_WARN, "failed to extract %s", e.rel_path.c_str());
            }
        }

        WriteStamp(project_root, stamp);
        return copied;
    }

    /// 决定项目根目录：APK assets 优先（正式出包），退回设备上的目录（adb push
    /// 的调试形态，此时不需要重新解包）。
    std::filesystem::path ResolveProjectRoot(android_app* app)
    {
        const char* internal = app->activity != nullptr ? app->activity->internalDataPath : nullptr;
        if (internal != nullptr && internal[0] != '\0')
        {
            g_files_dir = internal;
        }
        else
        {
            g_files_dir = "/data/local/tmp";
        }

        const std::filesystem::path candidate = std::filesystem::path(g_files_dir) / k_project_dir_name;

        if (app->activity != nullptr && app->activity->assetManager != nullptr)
        {
            const long long copied = ExtractPackagedAssets(app->activity->assetManager, candidate);
            if (copied >= 0)
            {
                AndroidLog(ANDROID_LOG_INFO, "extracted %lld asset files into %s",
                           copied, candidate.c_str());
                return candidate;
            }
        }

        std::error_code ec;
        if (std::filesystem::exists(candidate / "Build", ec))
        {
            AndroidLog(ANDROID_LOG_INFO, "using project already on device: %s", candidate.c_str());
            return candidate;
        }

        return candidate;  // 交给 ZPlayer_Boot 报 ProjectNotFound，错误更明确
    }

    bool BootEngine(android_app* app)
    {
        if (ZPlayer_IsRunning())
        {
            return true;
        }

        const std::filesystem::path project_root = ResolveProjectRoot(app);
        g_project_root = project_root.string();

        int width  = 0;
        int height = 0;
        if (app->window != nullptr)
        {
            width  = ANativeWindow_getWidth(app->window);
            height = ANativeWindow_getHeight(app->window);
        }

        ZPlayerBootDesc desc {};
        desc.project_path   = g_project_root.c_str();
        desc.writable_dir   = g_files_dir.c_str();
        desc.native_window  = app->window;
        desc.asset_manager  = app->activity != nullptr ? app->activity->assetManager : nullptr;
        desc.preferred_rhi  = ZPlayer_RHI_Default;
        desc.surface_width  = width;
        desc.surface_height = height;

        const ZPlayerResult result = ZPlayer_Boot(&desc);
        if (result != ZPlayer_OK)
        {
            AndroidLog(ANDROID_LOG_ERROR, "ZPlayer_Boot failed (%d) for %s",
                       static_cast<int>(result), g_project_root.c_str());
            return false;
        }
        return true;
    }

    /// 把 AInputEvent 的触摸翻译成 ZPlayer_Touch。
    ///
    /// 多指是这里唯一容易写错的地方：Android 把"第二根及以后的手指按下/抬起"
    /// 报成 POINTER_DOWN / POINTER_UP，而**同一个事件里其余手指的位置其实是
    /// 移动**。只按 masked action 给所有指针派发同一种事件，就会出现"第二根
    /// 手指按下时第一根手指被当成抬起"这类怪现象。
    int32_t OnInputEvent(android_app* /*app*/, AInputEvent* event)
    {
        if (event == nullptr || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        {
            return 0;  // 未处理：按键等交回系统默认行为
        }

        const int32_t masked = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;

        int action       = -1;
        int action_index = -1;  // 只对 POINTER_DOWN / POINTER_UP 有意义
        switch (masked)
        {
            case AMOTION_EVENT_ACTION_DOWN:
                action = ZPlayer_Touch_Down;
                action_index = 0;
                break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                action = ZPlayer_Touch_Down;
                action_index = AMotionEvent_getActionIndex(event);
                break;
            case AMOTION_EVENT_ACTION_UP:
                action = ZPlayer_Touch_Up;
                action_index = 0;
                break;
            case AMOTION_EVENT_ACTION_POINTER_UP:
                action = ZPlayer_Touch_Up;
                action_index = AMotionEvent_getActionIndex(event);
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                action = ZPlayer_Touch_Move;
                break;
            case AMOTION_EVENT_ACTION_CANCEL:
                action = ZPlayer_Touch_Cancel;
                break;
            default:
                return 0;
        }

        const size_t pointer_count = AMotionEvent_getPointerCount(event);
        for (size_t i = 0; i < pointer_count; ++i)
        {
            const int this_action =
                (action_index >= 0 && static_cast<int>(i) != action_index) ? ZPlayer_Touch_Move : action;

            ZPlayer_Touch(static_cast<int>(AMotionEvent_getPointerId(event, i)),
                          this_action,
                          AMotionEvent_getX(event, i),
                          AMotionEvent_getY(event, i));
        }
        return 1;  // 已处理
    }

    void OnAppCmd(android_app* app, int32_t cmd)
    {
        switch (cmd)
        {
            case APP_CMD_INIT_WINDOW:
                if (app->window != nullptr)
                {
                    if (!BootEngine(app))
                    {
                        g_destroy_requested = true;
                        break;
                    }
                    // 窗口就绪之后才认为可以渲染：Vulkan 的 surface 需要
                    // ANativeWindow，Boot 早于第一个 INIT_WINDOW 会拿不到。
                    ZPlayer_SetNativeWindow(app->window);
                    ZPlayer_Resize(ANativeWindow_getWidth(app->window),
                                   ANativeWindow_getHeight(app->window));
                    g_window_ready = true;
                }
                break;

            case APP_CMD_TERM_WINDOW:
                // ANativeWindow 即将失效。必须先让引擎放手：继续拿它 present
                // 是 Android 上最典型的 Vulkan 崩溃点（surface lost / 野指针）。
                ZPlayer_SetNativeWindow(nullptr);
                g_window_ready = false;
                break;

            case APP_CMD_GAINED_FOCUS:
                ZPlayer_SetFocus(1);
                ZPlayer_Resume();
                break;

            case APP_CMD_LOST_FOCUS:
                ZPlayer_Pause();
                ZPlayer_SetFocus(0);
                break;

            case APP_CMD_DESTROY:
                g_destroy_requested = true;
                break;

            default:
                break;
        }
    }
}  // namespace

extern "C" void android_main(struct android_app* app)
{
    app->onAppCmd      = OnAppCmd;
    app->onInputEvent  = OnInputEvent;

    // 引擎不在 android_main 里启动：此刻还没有 ANativeWindow（APP_CMD_INIT_WINDOW
    // 才会给）。Boot 推迟到第一次 INIT_WINDOW，见 OnAppCmd。

    while (!g_destroy_requested)
    {
        int   events = 0;
        void* source = nullptr;

        // 有窗口时用 0 超时（尽快进下一帧），没窗口时阻塞等事件（-1），
        // 免得后台空转烧电。
        const int timeout = g_window_ready ? 0 : -1;
        while (ALooper_pollAll(timeout, nullptr, &events, &source) >= 0)
        {
            if (source != nullptr)
            {
                auto* poll_source = static_cast<android_poll_source*>(source);
                poll_source->process(app, poll_source);
            }
            if (app->destroyRequested != 0)
            {
                g_destroy_requested = true;
                break;
            }
        }

        if (g_destroy_requested)
        {
            break;
        }

        if (g_window_ready)
        {
            // 传 0：让 Player 壳自己按 steady_clock 算 delta。Android 的帧回调
            // 不保证给出可靠的时间差，自算更稳。
            if (ZPlayer_Tick(0.0f) == 0)
            {
                g_destroy_requested = true;
            }
        }
    }

    ZPlayer_Shutdown();

    // native_app_glue 的约定：让 Activity finish，否则回退键只会黑屏。
    ANativeActivity_finish(app->activity);
}
