#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Platform/MacOS/MacOSApplication.h"

#if defined(__APPLE__)

    #define Component AppleComponent
    #import <Cocoa/Cocoa.h>
    #import <CoreGraphics/CoreGraphics.h>
    #undef Component

    #include <chrono>

namespace
{
    // NSApplication 只需初始化一次（sharedApplication 是进程级单例）。
    void SetupCocoaOnce()
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // 最小主菜单：没有它 Cmd+Q / Cmd+C / Cmd+V / Cmd+A 全是死键。
        NSMenu*      menubar     = [[NSMenu alloc] init];
        NSMenuItem*  app_item    = [[NSMenuItem alloc] init];
        NSMenu*      app_menu    = [[NSMenu alloc] initWithTitle:@"ZEngine"];
        NSMenuItem*  quit_item   = [[NSMenuItem alloc] initWithTitle:@"Quit ZEditor"
                                                              action:@selector(terminate:)
                                                       keyEquivalent:@"q"];
        [app_menu addItem:quit_item];
        [app_item setSubmenu:app_menu];
        [menubar addItem:app_item];

        NSMenuItem* edit_item = [[NSMenuItem alloc] init];
        NSMenu*     edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [edit_menu addItem:[[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                       action:@selector(cut:)
                                                keyEquivalent:@"x"] autorelease]];
        [edit_menu addItem:[[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                       action:@selector(copy:)
                                                keyEquivalent:@"c"] autorelease]];
        [edit_menu addItem:[[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                       action:@selector(paste:)
                                                keyEquivalent:@"v"] autorelease]];
        [edit_menu addItem:[[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                       action:@selector(selectAll:)
                                                keyEquivalent:@"a"] autorelease]];
        [edit_item setSubmenu:edit_menu];
        [menubar addItem:edit_item];

        [NSApp setMainMenu:menubar];
        // NSApp / menu 层级已经持有这些对象，释放本地引用。
        [menubar release];
        [app_item release];
        [app_menu release];
        [quit_item release];
        [edit_item release];
        [edit_menu release];

        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }

    // Cocoa 屏幕坐标原点在左下角，转成左上原点（同 Win32 / GLFW 约定）。
    GenericApplication::MonitorInfo WorkAreaFromScreen(NSScreen* screen, CGFloat global_height)
    {
        GenericApplication::MonitorInfo info {0, 0, 0, 0};
        if (screen == nil)
        {
            return info;
        }
        const NSRect frame = screen.visibleFrame;  // 已排除菜单栏 / Dock
        info.x      = static_cast<int>(frame.origin.x);
        info.y      = static_cast<int>(global_height - frame.origin.y - frame.size.height);
        info.width  = static_cast<int>(frame.size.width);
        info.height = static_cast<int>(frame.size.height);
        return info;
    }

    CGFloat GlobalScreenHeight()
    {
        NSScreen* primary = NSScreen.screens.firstObject;
        return primary != nil ? primary.frame.size.height : 0.0;
    }
}  // namespace

bool MacOSApplication::s_CocoaInitialized = false;

MacOSApplication::MacOSApplication() = default;

MacOSApplication::~MacOSApplication()
{
    Shutdown();
}

bool MacOSApplication::Initialize(const char* title, int width, int height)
{
    if (!s_CocoaInitialized)
    {
        SetupCocoaOnce();
        s_CocoaInitialized = true;
    }

    auto window = std::make_unique<MacOSWindow>();
    if (!window->Initialize(title, width, height, /*decorated=*/true, /*visible=*/false))
    {
        return false;
    }
    window->SetApplication(this);
    m_MainWindow = std::move(window);
    return true;
}

void MacOSApplication::Shutdown()
{
    m_OwnedChildren.clear();
    m_MainWindow.reset();
}

void MacOSApplication::PollEvents()
{
    @autoreleasepool
    {
        while (true)
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil)
            {
                break;
            }
            [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
}

bool MacOSApplication::ShouldClose() const
{
    if (m_ShouldClose)
    {
        return true;
    }
    return m_MainWindow != nullptr && m_MainWindow->ShouldClose();
}

void MacOSApplication::RequestClose()
{
    m_ShouldClose = true;
    if (m_MainWindow != nullptr)
    {
        m_MainWindow->SetShouldClose(true);
    }
}

void MacOSApplication::SetTitle(const char* title)
{
    if (m_MainWindow != nullptr)
    {
        m_MainWindow->SetTitle(title);
    }
}

void MacOSApplication::ShowMainWindow()
{
    if (m_MainWindow != nullptr)
    {
        m_MainWindow->Show();
    }
}

void MacOSApplication::SetCursorMode(bool capture)
{
    m_CursorCaptured = capture;
    if (capture)
    {
        // 解除鼠标与光标的联动（原始输入 / 虚拟鼠标模式），并隐藏系统光标。
        CGAssociateMouseAndMouseCursorPosition(static_cast<boolean_t>(0));
        [NSCursor hide];
    }
    else
    {
        CGAssociateMouseAndMouseCursorPosition(static_cast<boolean_t>(1));
        [NSCursor unhide];
    }
}

void MacOSApplication::SetClipboardText(const char* text)
{
    if (text == nullptr)
    {
        return;
    }
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
}

std::string MacOSApplication::GetClipboardText()
{
    NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    return text != nil ? std::string(text.UTF8String) : std::string();
}

void* MacOSApplication::CreateStandardCursor(int shape)
{
    // shape: 0=arrow, 1=hand, 2=hresize, 3=vresize, 4=nwse-resize
    switch (shape)
    {
        case 1: return (__bridge void*)NSCursor.pointingHandCursor;
        case 2: return (__bridge void*)NSCursor.resizeLeftRightCursor;
        case 3: return (__bridge void*)NSCursor.resizeUpDownCursor;
        case 4: return (__bridge void*)NSCursor.crosshairCursor;
        default: return (__bridge void*)NSCursor.arrowCursor;
    }
}

void MacOSApplication::SetCursor(void* window_handle, void* cursor)
{
    (void)window_handle;
    if (cursor != nullptr)
    {
        [((__bridge NSCursor*)cursor) set];
    }
}

double MacOSApplication::GetTime()
{
    static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count();
}

GenericWindow* MacOSApplication::CreateChildWindow(const char* title,
                                                   int         width,
                                                   int         height,
                                                   int         pos_x,
                                                   int         pos_y,
                                                   bool        decorated)
{
    auto   window = std::make_unique<MacOSWindow>();
    void*  parent = m_MainWindow != nullptr ? m_MainWindow->GetNativeHandle() : nullptr;
    if (!window->Initialize(title, width, height, decorated, /*visible=*/true, /*focus_on_show=*/false, parent))
    {
        return nullptr;
    }
    window->SetPosition(pos_x, pos_y);
    window->SetApplication(this);

    GenericWindow* result = window.get();
    m_OwnedChildren.push_back(std::move(window));
    return result;
}

void MacOSApplication::DestroyChildWindow(GenericWindow* window)
{
    for (auto it = m_OwnedChildren.begin(); it != m_OwnedChildren.end(); ++it)
    {
        if (it->get() == window)
        {
            m_OwnedChildren.erase(it);
            return;
        }
    }
}

GenericWindow* MacOSApplication::GetMainWindow() const
{
    return m_MainWindow.get();
}

std::array<int, 2> MacOSApplication::GetFramebufferSize() const
{
    return m_MainWindow != nullptr ? m_MainWindow->GetFramebufferSize() : std::array<int, 2> {0, 0};
}

int MacOSApplication::GetMonitorCount() const
{
    return static_cast<int>(NSScreen.screens.count);
}

GenericApplication::MonitorInfo MacOSApplication::GetPrimaryMonitorWorkArea() const
{
    NSScreen* screen = NSScreen.mainScreen;
    if (screen == nil)
    {
        screen = NSScreen.screens.firstObject;
    }
    return WorkAreaFromScreen(screen, GlobalScreenHeight());
}

GenericApplication::MonitorInfo MacOSApplication::GetMonitorWorkArea(int monitor_index) const
{
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (monitor_index < 0 || monitor_index >= static_cast<int>(screens.count))
    {
        return GetPrimaryMonitorWorkArea();
    }
    return WorkAreaFromScreen(screens[monitor_index], GlobalScreenHeight());
}

GenericApplication::MonitorInfo MacOSApplication::GetMonitorWorkAreaForWindow(GenericWindow* window) const
{
    if (window == nullptr)
    {
        return GetPrimaryMonitorWorkArea();
    }
    auto   position = window->GetPosition();
    auto   size = window->GetSize();
    const int center_x = position[0] + size[0] / 2;
    const int center_y = position[1] + size[1] / 2;

    const CGFloat global_height = GlobalScreenHeight();
    NSArray<NSScreen*>* screens = NSScreen.screens;
    for (NSScreen* screen in screens)
    {
        const NSRect frame = screen.visibleFrame;
        const int    x = static_cast<int>(frame.origin.x);
        const int    y = static_cast<int>(global_height - frame.origin.y - frame.size.height);
        const int    w = static_cast<int>(frame.size.width);
        const int    h = static_cast<int>(frame.size.height);
        if (center_x >= x && center_x < x + w && center_y >= y && center_y < y + h)
        {
            return {x, y, w, h};
        }
    }
    return GetPrimaryMonitorWorkArea();
}

#endif  // __APPLE__
