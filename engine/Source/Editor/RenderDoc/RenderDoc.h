#pragma once

#include <cstdint>

namespace Runtime
{
    class RenderDoc
    {
    public:
        static void Init();
        static void Load();

        static bool IsInstalled();
        static bool IsLoaded();
        static bool IsSupported();

        static void RequestCapture();
        // 延后 `delay_frames` 帧再抓（启动参数里排的抓帧：资产要几帧才加载完）；`launch_ui = false` 时抓完
        // 不拉起 qrenderdoc（无人值守跑，配合 `renderdoccmd thumb` 导出画面）。
        static void RequestCapture(uint32_t delay_frames, bool launch_ui);
        static void OnPreFrame();
        static void OnPostFrame();

        static void BeginFrameCapture();
        static void EndFrameCapture();
    };
}  // namespace Runtime
