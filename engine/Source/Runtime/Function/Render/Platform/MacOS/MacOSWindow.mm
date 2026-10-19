#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Platform/MacOS/MacOSWindow.h"
#include "Runtime/Function/Render/Platform/Generic/GenericApplication.h"
#include "Runtime/Function/Input/KeyCodes.h"

#if defined(__APPLE__)

    #define Component AppleComponent
    #import <Cocoa/Cocoa.h>
    #import <Carbon/Carbon.h>
    #undef Component

namespace
{
    // macOS 修饰键 -> KeyCodes::Mod（GLFW 位定义）
    int MapModifiers(NSEventModifierFlags flags)
    {
        int mods = 0;
        if ((flags & NSEventModifierFlagShift) != 0)     mods |= KeyCodes::MOD_SHIFT_VAL;
        if ((flags & NSEventModifierFlagControl) != 0)   mods |= KeyCodes::MOD_CONTROL_VAL;
        if ((flags & NSEventModifierFlagOption) != 0)    mods |= KeyCodes::MOD_ALT_VAL;
        if ((flags & NSEventModifierFlagCommand) != 0)   mods |= KeyCodes::MOD_SUPER_VAL;
        if ((flags & NSEventModifierFlagCapsLock) != 0)  mods |= KeyCodes::MOD_CAPS_LOCK_VAL;
        return mods;
    }

    // macOS 虚拟键码（Events.h 的 kVK_*）-> KeyCodes::Key（GLFW 兼容值）
    int MapKeyCode(unsigned short code)
    {
        switch (code)
        {
            // ---- 字母 ----
            case 0x00: return static_cast<int>(KeyCodes::Key::A);
            case 0x01: return static_cast<int>(KeyCodes::Key::S);
            case 0x02: return static_cast<int>(KeyCodes::Key::D);
            case 0x03: return static_cast<int>(KeyCodes::Key::F);
            case 0x04: return static_cast<int>(KeyCodes::Key::H);
            case 0x05: return static_cast<int>(KeyCodes::Key::G);
            case 0x06: return static_cast<int>(KeyCodes::Key::Z);
            case 0x07: return static_cast<int>(KeyCodes::Key::X);
            case 0x08: return static_cast<int>(KeyCodes::Key::C);
            case 0x09: return static_cast<int>(KeyCodes::Key::V);
            case 0x0B: return static_cast<int>(KeyCodes::Key::B);
            case 0x0C: return static_cast<int>(KeyCodes::Key::Q);
            case 0x0D: return static_cast<int>(KeyCodes::Key::W);
            case 0x0E: return static_cast<int>(KeyCodes::Key::E);
            case 0x0F: return static_cast<int>(KeyCodes::Key::R);
            case 0x10: return static_cast<int>(KeyCodes::Key::Y);
            case 0x11: return static_cast<int>(KeyCodes::Key::T);
            case 0x22: return static_cast<int>(KeyCodes::Key::I);
            case 0x1F: return static_cast<int>(KeyCodes::Key::O);
            case 0x23: return static_cast<int>(KeyCodes::Key::P);
            case 0x25: return static_cast<int>(KeyCodes::Key::L);
            case 0x26: return static_cast<int>(KeyCodes::Key::J);
            case 0x28: return static_cast<int>(KeyCodes::Key::K);
            case 0x2D: return static_cast<int>(KeyCodes::Key::N);
            case 0x2E: return static_cast<int>(KeyCodes::Key::M);

            // ---- 数字 / 符号 ----
            case 0x12: return static_cast<int>(KeyCodes::Key::D1);
            case 0x13: return static_cast<int>(KeyCodes::Key::D2);
            case 0x14: return static_cast<int>(KeyCodes::Key::D3);
            case 0x15: return static_cast<int>(KeyCodes::Key::D4);
            case 0x17: return static_cast<int>(KeyCodes::Key::D5);
            case 0x16: return static_cast<int>(KeyCodes::Key::D6);
            case 0x1A: return static_cast<int>(KeyCodes::Key::D7);
            case 0x1C: return static_cast<int>(KeyCodes::Key::D8);
            case 0x19: return static_cast<int>(KeyCodes::Key::D9);
            case 0x1D: return static_cast<int>(KeyCodes::Key::D0);
            case 0x1B: return static_cast<int>(KeyCodes::Key::Minus);
            case 0x18: return static_cast<int>(KeyCodes::Key::Equal);
            case 0x21: return static_cast<int>(KeyCodes::Key::LeftBracket);
            case 0x1E: return static_cast<int>(KeyCodes::Key::RightBracket);
            case 0x2A: return static_cast<int>(KeyCodes::Key::Backslash);
            case 0x27: return static_cast<int>(KeyCodes::Key::Apostrophe);
            case 0x29: return static_cast<int>(KeyCodes::Key::Semicolon);
            case 0x2B: return static_cast<int>(KeyCodes::Key::Comma);
            case 0x2F: return static_cast<int>(KeyCodes::Key::Period);
            case 0x2C: return static_cast<int>(KeyCodes::Key::Slash);
            case 0x32: return static_cast<int>(KeyCodes::Key::GraveAccent);

            // ---- 控制键 ----
            case 0x24: return static_cast<int>(KeyCodes::Key::Enter);
            case 0x30: return static_cast<int>(KeyCodes::Key::Tab);
            case 0x31: return static_cast<int>(KeyCodes::Key::Space);
            case 0x33: return static_cast<int>(KeyCodes::Key::Backspace);
            case 0x35: return static_cast<int>(KeyCodes::Key::Escape);
            case 0x39: return static_cast<int>(KeyCodes::Key::CapsLock);
            case 0x75: return static_cast<int>(KeyCodes::Key::Delete);
            case 0x47: return static_cast<int>(KeyCodes::Key::NumLock);
            case 0x72: return static_cast<int>(KeyCodes::Key::Insert);

            // ---- 方向 / 导航 ----
            case 0x7B: return static_cast<int>(KeyCodes::Key::Left);
            case 0x7C: return static_cast<int>(KeyCodes::Key::Right);
            case 0x7D: return static_cast<int>(KeyCodes::Key::Down);
            case 0x7E: return static_cast<int>(KeyCodes::Key::Up);
            case 0x73: return static_cast<int>(KeyCodes::Key::Home);
            case 0x77: return static_cast<int>(KeyCodes::Key::End);
            case 0x74: return static_cast<int>(KeyCodes::Key::PageUp);
            case 0x79: return static_cast<int>(KeyCodes::Key::PageDown);

            // ---- 功能键 ----
            case 0x7A: return static_cast<int>(KeyCodes::Key::F1);
            case 0x78: return static_cast<int>(KeyCodes::Key::F2);
            case 0x63: return static_cast<int>(KeyCodes::Key::F3);
            case 0x76: return static_cast<int>(KeyCodes::Key::F4);
            case 0x60: return static_cast<int>(KeyCodes::Key::F5);
            case 0x61: return static_cast<int>(KeyCodes::Key::F6);
            case 0x62: return static_cast<int>(KeyCodes::Key::F7);
            case 0x64: return static_cast<int>(KeyCodes::Key::F8);
            case 0x65: return static_cast<int>(KeyCodes::Key::F9);
            case 0x6D: return static_cast<int>(KeyCodes::Key::F10);
            case 0x67: return static_cast<int>(KeyCodes::Key::F11);
            case 0x6F: return static_cast<int>(KeyCodes::Key::F12);
            case 0x69: return static_cast<int>(KeyCodes::Key::F13);
            case 0x6B: return static_cast<int>(KeyCodes::Key::F14);
            case 0x71: return static_cast<int>(KeyCodes::Key::F15);
            case 0x6A: return static_cast<int>(KeyCodes::Key::F16);
            case 0x40: return static_cast<int>(KeyCodes::Key::F17);
            case 0x4F: return static_cast<int>(KeyCodes::Key::F18);
            case 0x50: return static_cast<int>(KeyCodes::Key::F19);
            case 0x5A: return static_cast<int>(KeyCodes::Key::F20);

            // ---- 小键盘 ----
            case 0x52: return static_cast<int>(KeyCodes::Key::KP0);
            case 0x53: return static_cast<int>(KeyCodes::Key::KP1);
            case 0x54: return static_cast<int>(KeyCodes::Key::KP2);
            case 0x55: return static_cast<int>(KeyCodes::Key::KP3);
            case 0x56: return static_cast<int>(KeyCodes::Key::KP4);
            case 0x57: return static_cast<int>(KeyCodes::Key::KP5);
            case 0x58: return static_cast<int>(KeyCodes::Key::KP6);
            case 0x59: return static_cast<int>(KeyCodes::Key::KP7);
            case 0x5B: return static_cast<int>(KeyCodes::Key::KP8);
            case 0x5C: return static_cast<int>(KeyCodes::Key::KP9);
            case 0x41: return static_cast<int>(KeyCodes::Key::KPDecimal);
            case 0x43: return static_cast<int>(KeyCodes::Key::KPMultiply);
            case 0x45: return static_cast<int>(KeyCodes::Key::KPAdd);
            case 0x4B: return static_cast<int>(KeyCodes::Key::KPDivide);
            case 0x4E: return static_cast<int>(KeyCodes::Key::KPSubtract);
            case 0x51: return static_cast<int>(KeyCodes::Key::KPEqual);
            case 0x4C: return static_cast<int>(KeyCodes::Key::KPEnter);

            // ---- 修饰键（modifier 单独 按下/抬起）----
            case 0x38: return static_cast<int>(KeyCodes::Key::LeftShift);
            case 0x3C: return static_cast<int>(KeyCodes::Key::RightShift);
            case 0x3B: return static_cast<int>(KeyCodes::Key::LeftControl);
            case 0x3E: return static_cast<int>(KeyCodes::Key::RightControl);
            case 0x3A: return static_cast<int>(KeyCodes::Key::LeftAlt);
            case 0x3D: return static_cast<int>(KeyCodes::Key::RightAlt);
            case 0x37: return static_cast<int>(KeyCodes::Key::LeftSuper);
            case 0x36: return static_cast<int>(KeyCodes::Key::RightSuper);

            default: return 0;
        }
    }
}  // namespace

// ---------------------------------------------------------------------------
// 承载渲染 layer 与输入事件的 Cocoa 视图
// ---------------------------------------------------------------------------
@interface ZEngineCocoaView : NSView <NSTextInputClient>
{
@public
    MacOSWindow*           owner;
    NSEventModifierFlags   lastFlags;
    NSMutableAttributedString* markedText;
}
@end

@implementation ZEngineCocoaView

- (instancetype)initWithFrame:(NSRect)frame owner:(MacOSWindow*)inOwner
{
    self = [super initWithFrame:frame];
    if (self != nil)
    {
        owner      = inOwner;
        lastFlags  = 0;
        markedText = [[NSMutableAttributedString alloc] init];
        [self registerForDraggedTypes:[NSArray arrayWithObject:NSPasteboardTypeFileURL]];
    }
    return self;
}

- (void)dealloc
{
    [markedText release];
    [super dealloc];
}

- (BOOL)acceptsFirstResponder  { return YES; }
- (BOOL)isOpaque               { return YES; }
- (BOOL)acceptsMouseMovedEvents { return YES; }

- (void)updateTrackingAreas
{
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:area];
    [area release];
    [super updateTrackingAreas];
}

// ---- 键盘 ----

- (void)sendKey:(NSEvent*)event pressed:(BOOL)pressed
{
    const int keycode = MapKeyCode(event.keyCode);
    if (keycode == 0 || owner == nullptr)
    {
        return;
    }
    // lParam 里的 scancode 在 macOS 没有等价物，用虚拟键码代替。
    const int scancode   = static_cast<int>(event.keyCode);
    const int mods       = MapModifiers(event.modifierFlags);
    const int action     = pressed ? (event.isARepeat ? 2 : 1) : 0;

    if (auto* app = owner->GetApplication())
    {
        app->UpdateKeyState(keycode, pressed ? 1 : 0);
    }
    for (auto& cb : owner->OnKey)
    {
        cb(keycode, scancode, action, mods);
    }
}

- (void)keyDown:(NSEvent*)event
{
    [self sendKey:event pressed:YES];
    // 交给文本系统：可输入字符走 insertText:，输入法走 setMarkedText:
    [self interpretKeyEvents:[NSArray arrayWithObject:event]];
}

- (void)keyUp:(NSEvent*)event
{
    [self sendKey:event pressed:NO];
}

- (void)flagsChanged:(NSEvent*)event
{
    const NSEventModifierFlags flags = event.modifierFlags;
    const NSEventModifierFlags diff = flags ^ lastFlags;
    lastFlags = flags;
    if (owner == nullptr || diff == 0)
    {
        return;
    }

    struct FlagMapping { NSEventModifierFlags flag; unsigned short keyCode; };
    const FlagMapping mappings[] = {
        {NSEventModifierFlagShift,   0x38},
        {NSEventModifierFlagControl, 0x3B},
        {NSEventModifierFlagOption,  0x3A},
        {NSEventModifierFlagCommand, 0x37},
        {NSEventModifierFlagCapsLock, 0x39},
    };
    const int mods = MapModifiers(flags);
    for (const FlagMapping& mapping : mappings)
    {
        if ((diff & mapping.flag) == 0)
        {
            continue;
        }
        const int  keycode  = MapKeyCode(mapping.keyCode);
        const bool pressed  = (flags & mapping.flag) != 0;
        if (keycode == 0)
        {
            continue;
        }
        if (auto* app = owner->GetApplication())
        {
            app->UpdateKeyState(keycode, pressed ? 1 : 0);
        }
        for (auto& cb : owner->OnKey)
        {
            cb(keycode, mapping.keyCode, pressed ? 1 : 0, mods);
        }
    }
}

// ---- NSTextInputClient（保证中文/日文输入法可用）----

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
    NSString*  text   = nil;
    NSUInteger length = 0;
    (void)replacementRange;
    if ([string isKindOfClass:[NSAttributedString class]])
    {
        text = ((NSAttributedString*)string).string;
    }
    else if ([string isKindOfClass:[NSString class]])
    {
        text = (NSString*)string;
    }
    if (text == nil || owner == nullptr)
    {
        return;
    }
    [markedText setAttributedString:[[[NSAttributedString alloc] initWithString:@""] autorelease]];

    length = text.length;
    const int mods = MapModifiers([[NSApp currentEvent] modifierFlags]);
    for (NSUInteger i = 0; i < length; ++i)
    {
        const unichar  character = [text characterAtIndex:i];
        const unsigned codepoint = static_cast<unsigned>(character);
        for (auto& cb : owner->OnChar)     cb(codepoint);
        for (auto& cb : owner->OnCharMods) cb(static_cast<int>(codepoint), static_cast<unsigned>(mods));
    }
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange
{
    (void)selectedRange;
    (void)replacementRange;
    NSAttributedString* attributed = nil;
    if ([string isKindOfClass:[NSAttributedString class]])
    {
        attributed = (NSAttributedString*)string;
    }
    else if ([string isKindOfClass:[NSString class]])
    {
        attributed = [[[NSAttributedString alloc] initWithString:(NSString*)string] autorelease];
    }
    if (attributed == nil)
    {
        return;
    }
    [markedText setAttributedString:attributed];
}

- (void)unmarkText
{
    [markedText setAttributedString:[[[NSAttributedString alloc] initWithString:@""] autorelease]];
}

- (BOOL)hasMarkedText { return markedText.length > 0; }
- (NSRange)markedRange { return NSMakeRange(0, markedText.length); }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }

- (NSArray*)validAttributesForMarkedText { return [NSArray array]; }

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    (void)range;
    if (actualRange != nullptr)
    {
        *actualRange = NSMakeRange(NSNotFound, 0);
    }
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    (void)point;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    (void)range;
    if (actualRange != nullptr)
    {
        *actualRange = NSMakeRange(NSNotFound, 0);
    }
    NSRect rect = self.bounds;
    rect.origin = [self.window convertPointToScreen:self.frame.origin];
    return rect;
}

- (void)doCommandBySelector:(SEL)selector
{
    // 物理按键已经在 keyDown 里通过 OnKey 派发过了，这里不再重复（避免 Enter /
    // Backspace 等触发两次）。保留实现只是为了满足 NSTextInputClient 协议。
    (void)selector;
}

// ---- 鼠标 ----

- (void)sendMouseButton:(NSEvent*)event button:(int)button pressed:(BOOL)pressed
{
    if (owner == nullptr)
    {
        return;
    }
    const int mods = MapModifiers(event.modifierFlags);
    if (auto* app = owner->GetApplication())
    {
        app->UpdateMouseState(button, pressed ? 1 : 0);
    }
    for (auto& cb : owner->OnMouseButton)
    {
        cb(button, pressed ? 1 : 0, mods);
    }
}

- (void)sendCursorPos:(NSEvent*)event
{
    if (owner == nullptr)
    {
        return;
    }
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    point.y = self.bounds.size.height - point.y;  // 左上原点（同 Win32 客户区 / GLFW）
    // Scale to physical pixels: the editor's ZSlate layout and input host work
    // in device pixels (WindowsWindow contract), see MacOSWindow::GetSize().
    const CGFloat scale = self.window != nil && self.window.backingScaleFactor > 0.0
                              ? self.window.backingScaleFactor
                              : 1.0;
    for (auto& cb : owner->OnCursorPos)
        cb(static_cast<double>(point.x * scale), static_cast<double>(point.y * scale));
}

- (void)mouseDown:(NSEvent*)event       { [self sendMouseButton:event button:0 pressed:YES]; }
- (void)mouseUp:(NSEvent*)event         { [self sendMouseButton:event button:0 pressed:NO]; }
- (void)rightMouseDown:(NSEvent*)event  { [self sendMouseButton:event button:1 pressed:YES]; }
- (void)rightMouseUp:(NSEvent*)event    { [self sendMouseButton:event button:1 pressed:NO]; }
- (void)otherMouseDown:(NSEvent*)event  { [self sendMouseButton:event button:2 pressed:YES]; }
- (void)otherMouseUp:(NSEvent*)event    { [self sendMouseButton:event button:2 pressed:NO]; }

- (void)mouseMoved:(NSEvent*)event      { [self sendCursorPos:event]; }
- (void)mouseDragged:(NSEvent*)event    { [self sendCursorPos:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendCursorPos:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendCursorPos:event]; }

- (void)mouseEntered:(NSEvent*)event
{
    (void)event;
    if (owner == nullptr) return;
    for (auto& cb : owner->OnCursorEnter) cb(1);
}

- (void)mouseExited:(NSEvent*)event
{
    (void)event;
    if (owner == nullptr) return;
    for (auto& cb : owner->OnCursorEnter) cb(0);
}

- (void)scrollWheel:(NSEvent*)event
{
    if (owner == nullptr) return;
    for (auto& cb : owner->OnScroll) cb(static_cast<double>(event.deltaX), static_cast<double>(event.deltaY));
}

// ---- 尺寸变化 ----

- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    if (owner == nullptr) return;
    // Report physical pixels (see MacOSWindow::GetSize for the convention).
    const CGFloat scale = self.window != nil && self.window.backingScaleFactor > 0.0
                              ? self.window.backingScaleFactor
                              : 1.0;
    for (auto& cb : owner->OnWindowSize)
        cb(static_cast<int>(newSize.width * scale), static_cast<int>(newSize.height * scale));
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (owner == nullptr) return;
    NSSize size = self.bounds.size;
    for (auto& cb : owner->OnWindowSize) cb(static_cast<int>(size.width), static_cast<int>(size.height));
}

// ---- 拖放 ----

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    if (owner == nullptr)
    {
        return NO;
    }
    NSArray* urls = [sender.draggingPasteboard readObjectsForClasses:[NSArray arrayWithObject:[NSURL class]] options:nil];
    std::vector<std::string> paths;
    std::vector<const char*> raw_paths;
    for (NSURL* url in urls)
    {
        NSString* path = url.path;
        if (path == nil)
        {
            continue;
        }
        paths.emplace_back(path.UTF8String);
    }
    for (const std::string& path : paths)
    {
        raw_paths.push_back(path.c_str());
    }
    if (raw_paths.empty())
    {
        return NO;
    }
    for (auto& cb : owner->OnDrop)
    {
        cb(static_cast<int>(raw_paths.size()), raw_paths.data());
    }
    return YES;
}

@end

// ---------------------------------------------------------------------------
// 窗口代理
// ---------------------------------------------------------------------------
@interface ZEngineWindowDelegate : NSObject <NSWindowDelegate>
{
@public
    MacOSWindow* owner;
}
@end

@implementation ZEngineWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    (void)sender;
    if (owner == nullptr)
    {
        return YES;
    }
    for (auto& cb : owner->OnWindowClose) cb();
    owner->SetShouldClose(true);
    return YES;
}

- (void)windowDidResize:(NSNotification*)notification
{
    (void)notification;
    if (owner == nullptr) return;
    auto size = owner->GetSize();
    for (auto& cb : owner->OnWindowSize) cb(size[0], size[1]);
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    (void)notification;
    if (owner == nullptr) return;
    for (auto& cb : owner->OnWindowFocus) cb(1);
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (owner == nullptr) return;
    for (auto& cb : owner->OnWindowFocus) cb(0);
}

- (void)windowDidExpose:(NSNotification*)notification
{
    (void)notification;
    if (owner == nullptr) return;
    for (auto& cb : owner->OnWindowRefresh) cb();
}

@end

// ---------------------------------------------------------------------------
// MacOSWindow
// ---------------------------------------------------------------------------

MacOSWindow::~MacOSWindow()
{
    Destroy();
}

bool MacOSWindow::Initialize(const char* title,
                             int         width,
                             int         height,
                             bool        decorated,
                             bool        visible,
                             bool        focus_on_show,
                             void*       parent_window)
{
    @autoreleasepool
    {
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (decorated)
        {
            style |= NSWindowStyleMaskResizable;
        }

        NSRect content_rect = NSMakeRect(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));
        NSWindow* window = [[NSWindow alloc] initWithContentRect:content_rect
                                                       styleMask:style
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        if (window == nil)
        {
            return false;
        }

        window.title                = title != nullptr ? [NSString stringWithUTF8String:title] : @"ZEngine";
        window.releasedWhenClosed   = NO;

        ZEngineCocoaView* view = [[ZEngineCocoaView alloc] initWithFrame:content_rect owner:this];
        window.contentView = view;   // contentView 会 retain
        [view release];

        ZEngineWindowDelegate* delegate = [[ZEngineWindowDelegate alloc] init];
        delegate->owner = this;
        window.delegate = delegate;  // delegate 是 assign，Destroy() 里手动 release

        m_NativeWindow = (__bridge void*)window;

        if (visible)
        {
            if (focus_on_show)
            {
                [window makeKeyAndOrderFront:nil];
            }
            else
            {
                [window orderFront:nil];
            }
        }

        if (parent_window != nullptr)
        {
            // 对齐 Win32 的 owner window 语义：浮在 owner 之上、跟随 z 序。
            NSWindow* parent = (__bridge NSWindow*)parent_window;
            [parent addChildWindow:window ordered:NSWindowAbove];
        }
    }
    return true;
}

void* MacOSWindow::GetNativeHandle() const
{
    return m_NativeWindow;
}

void* MacOSWindow::GetNativeView() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return nullptr;
    }
    return (__bridge void*)window.contentView;
}

void MacOSWindow::SetTitle(const char* title)
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window != nil && title != nullptr)
    {
        window.title = [NSString stringWithUTF8String:title];
    }
}

void MacOSWindow::Show()
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window != nil)
    {
        [window makeKeyAndOrderFront:nil];
    }
}

void MacOSWindow::Hide()
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window != nil)
    {
        [window orderOut:nil];
    }
}

void MacOSWindow::Destroy()
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return;
    }
    m_NativeWindow = nullptr;

    id<NSWindowDelegate> delegate = window.delegate;
    [delegate release];
    window.delegate = nil;
    [window close];
    [window release];
}

bool MacOSWindow::IsVisible() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    return window != nil && window.isVisible;
}

std::array<int, 2> MacOSWindow::GetSize() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return {0, 0};
    }
    // Physical pixels, matching the WindowsWindow contract the editor relies on
    // (see EditorSlateHost: GetSize() and GetFramebufferSize() are both the
    // client area in device pixels, and ZSlate's layout space + input hit-testing
    // work in that space). Reporting logical points here made every UI element
    // render 2x too large on Retina (1280x720 layout mapped against a
    // 2560x1440 viewport).
    NSRect backing = [window.contentView convertRectToBacking:window.contentView.bounds];
    return {static_cast<int>(backing.size.width), static_cast<int>(backing.size.height)};
}

std::array<int, 2> MacOSWindow::GetPosition() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return {0, 0};
    }
    // Top-left origin (Cocoa is bottom-left), in physical pixels like Win32.
    NSPoint  origin = window.frame.origin;
    CGFloat  scale  = window.screen != nil ? window.screen.backingScaleFactor : 1.0;
    CGFloat  primary_height = NSScreen.screens.firstObject.frame.size.height;
    CGFloat  top_left_y = primary_height - origin.y - window.frame.size.height;
    return {static_cast<int>(origin.x * scale), static_cast<int>(top_left_y * scale)};
}

void MacOSWindow::SetSize(int width, int height)
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window != nil)
    {
        // Input is physical pixels (see GetSize); CAMetalLayer's host view works
        // in points, so divide the scale back out.
        CGFloat scale = window.backingScaleFactor > 0.0 ? window.backingScaleFactor : 1.0;
        [window setContentSize:NSMakeSize(static_cast<CGFloat>(width) / scale,
                                          static_cast<CGFloat>(height) / scale)];
    }
}

void MacOSWindow::SetPosition(int x, int y)
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return;
    }
    // Physical pixels in, points out (inverse of GetPosition).
    CGFloat scale = window.screen != nil ? window.screen.backingScaleFactor : 1.0;
    CGFloat primary_height = NSScreen.screens.firstObject.frame.size.height;
    NSPoint origin = NSMakePoint(static_cast<CGFloat>(x) / scale,
                                 primary_height - static_cast<CGFloat>(y) / scale - window.frame.size.height);
    [window setFrameOrigin:origin];
}

std::array<int, 2> MacOSWindow::GetFramebufferSize() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return {0, 0};
    }
    NSRect backing = [window.contentView convertRectToBacking:window.contentView.bounds];
    return {static_cast<int>(backing.size.width), static_cast<int>(backing.size.height)};
}

float MacOSWindow::GetDpiScale() const
{
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    const CGFloat scale = window != nil ? window.backingScaleFactor : 1.0;
    return scale > 0.0 ? static_cast<float>(scale) : 1.0f;
}

void MacOSWindow::GetCursorPos(double& x, double& y) const
{
    x = 0.0;
    y = 0.0;
    NSWindow* window = (__bridge NSWindow*)m_NativeWindow;
    if (window == nil)
    {
        return;
    }
    NSPoint point = window.mouseLocationOutsideOfEventStream;
    // Physical pixels, top-left origin -- same space as GetSize() / ZSlate layout.
    const CGFloat scale = window.backingScaleFactor > 0.0 ? window.backingScaleFactor : 1.0;
    x = static_cast<double>(point.x * scale);
    y = static_cast<double>((window.contentView.frame.size.height - point.y) * scale);
}

bool MacOSWindow::ShouldClose() const
{
    return m_ShouldClose;
}

void MacOSWindow::SetShouldClose(bool v)
{
    m_ShouldClose = v;
}

#endif  // __APPLE__
