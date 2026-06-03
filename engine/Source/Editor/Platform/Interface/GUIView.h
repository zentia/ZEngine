#pragma once

#include "Editor/LevelEditor/LevelInspector.h"
#include "EditorWindowsTypes.h"

class GUIView;
typedef std::vector<GUIView*> GUIViews;
class GUIView : public LevelInspector
{
private:
    /* data */
    bool m_AutoRepaint;
    bool m_NeedsRepaint;
#if Z_PLATFORM_WINDOWS
    HWND m_View;
#endif

public:
    GUIView(/* args */);
    ~GUIView();
    static void RepaintAll(bool performAutorepaint);
#if Z_PLATFORM_WINDOWS
    static void RepaintViews(bool performAutorepaint, const GUIViews& views);
    WindowsHandle_t getWindowHandle() const { return m_View; }
#endif

#if Z_PLATFORM_WINDOWS
    static int s_InsideRepaintLimit;
#endif
};