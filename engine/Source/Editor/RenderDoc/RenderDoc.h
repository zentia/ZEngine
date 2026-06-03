#pragma once

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
        static void OnPreFrame();
        static void OnPostFrame();

        static void BeginFrameCapture();
        static void EndFrameCapture();
    };
}  // namespace Runtime
