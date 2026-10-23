// =============================================================================
// ZEditorLauncher — macOS (AppKit) 实现
// -----------------------------------------------------------------------------
// 为什么这里是 .mm 而不是 .cpp：Launcher 是 ZEditor 里唯一一个"在 START_SYSTEM()
// 之前就必须有窗口"的地方，而 ZSlate 的 FSlateStandaloneApp 是 Win32 + D3D11 的
// （engine/Libraries/ZSlate/CMakeLists.txt 在非 Windows 上把 App/SlateStandaloneApp
//  整个从源文件列表里滤掉），macOS 上没有可复用的独立 Slate 宿主。所以这一版
// 直接用 AppKit 手搭窗口，不碰 ZSlate，也不碰 RHI。
//
// 它跟 Win32 版是同一套契约（见 ZEditorLauncher.h）：**它不打开项目**，只把选中的
// 套件 + .zproject 路径交回调用方，由 Main.cpp 重启本进程并带上 -p。原因是
// CommandSystem::Initialize() 是从 `_NSGetArgv()` 里读 -p 的（见 CommandSystem.cpp），
// 进程内改 argv 既不干净也不安全，重启是唯一可靠的办法 —— Win32 那边用
// ShellExecuteW 干的是同一件事，macOS 这边用 posix_spawn + POSIX_SPAWN_SETEXEC
// （不 fork：Cocoa 起来之后 fork 是不安全的，SETEXEC 直接换映像）。
//
// 顺带说明"双击 ZEditor 闪退"这件事：Launcher 不存在时，Unix 分支走的是
// `reportMissingProject()` —— 往 stderr 打一行字然后 return 1。双击（Finder/Launchpad）
// 启动的进程没有终端，那行字谁也看不见，进程静默退出 1，看起来就是崩溃。
// 有窗口才能把"ZEditor 需要一个项目"这件事真正送到人眼前。
// =============================================================================

#include "Editor/Launcher/ZEditorLauncher.h"
#include "Editor/ToolSuite/ToolSuite.h"
// ⚠️ 这个 .mm 被 CMake 设了 SKIP_PRECOMPILE_HEADERS（Editor/CMakeLists.txt），所以
// PCH 不会把 LOG_* 带进来 —— 必须显式包含 Macro.h，否则 `LOG_WARNING(ZEditor, ...)`
// 不会被展开成宏，而是被当成一次函数调用，`ZEditor` 就被解析成命名空间，
// 报 "unexpected namespace name"。
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

#include <mach-o/dyld.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace ZEditorLauncher
{
namespace
{
constexpr int kMaxRecent = 10;

// -----------------------------------------------------------------------------
// 目录
//
// 用 NSSearchPathForDirectoriesInDomains 而不是直接拼 ~/Library：~/Library 在
// 沙盒 / 网络家目录 / 重定向过的容器里不一定是对的，API 会给出当前进程真正该
// 用的那个。拿不到再退回 $HOME。
// -----------------------------------------------------------------------------
std::filesystem::path UserDirectory(NSSearchPathDirectory directory)
{
    @autoreleasepool
    {
        NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);
        if (dirs.count > 0)
        {
            NSString* first = dirs.firstObject;
            if (first != nil)
            {
                return std::filesystem::path(first.UTF8String);
            }
        }
    }
    return {};
}

std::filesystem::path ApplicationSupportDir()
{
    std::filesystem::path base = UserDirectory(NSApplicationSupportDirectory);
    if (!base.empty())
    {
        return base;
    }
    if (const char* home = std::getenv("HOME"))
    {
        return std::filesystem::path(home) / "Library" / "Application Support";
    }
    return {};
}

std::filesystem::path DocumentsDir()
{
    std::filesystem::path base = UserDirectory(NSDocumentDirectory);
    if (!base.empty())
    {
        return base;
    }
    if (const char* home = std::getenv("HOME"))
    {
        return std::filesystem::path(home) / "Documents";
    }
    return {};
}

std::filesystem::path RecentListPath()
{
    const std::filesystem::path base = ApplicationSupportDir();
    if (base.empty())
    {
        return {};
    }
    return base / "ZEngine" / "ZEditor" / "recent_projects.txt";
}

std::filesystem::path DefaultProjectsDir()
{
    const std::filesystem::path documents = DocumentsDir();
    if (documents.empty())
    {
        return {};
    }
    return documents / "ZEngine";
}

// -----------------------------------------------------------------------------
// 最近项目列表
//
// 存 UTF-8 纯文本，一行一条，跟 Win32 版同一个文件语义（两边不共用文件，但共用
// 格式，便于人工编辑/排查）。读取时丢掉已经不存在的条目：项目被移动或删掉之后
// 留在列表里，点下去只会得到一个打不开的编辑器。
// -----------------------------------------------------------------------------
std::vector<std::filesystem::path> LoadRecent(const std::filesystem::path& list_path)
{
    std::vector<std::filesystem::path> out;
    FILE* f = std::fopen(list_path.c_str(), "rb");
    if (f == nullptr)
    {
        return out;
    }

    std::string all;
    char   buf[4096];
    size_t read = 0;
    while ((read = std::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        all.append(buf, read);
    }
    std::fclose(f);

    size_t start = 0;
    while (start < all.size())
    {
        size_t end = all.find('\n', start);
        if (end == std::string::npos)
        {
            end = all.size();
        }
        std::string line = all.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            std::filesystem::path candidate(line);
            if (std::filesystem::exists(candidate))
            {
                out.push_back(std::move(candidate));
            }
        }
        start = end + 1;
    }
    return out;
}

void SaveRecent(const std::filesystem::path& list_path, const std::vector<std::filesystem::path>& list)
{
    std::error_code ec;
    std::filesystem::create_directories(list_path.parent_path(), ec);
    FILE* f = std::fopen(list_path.c_str(), "wb");
    if (f == nullptr)
    {
        return;
    }
    for (const auto& entry : list)
    {
        const std::string line = entry.string() + "\n";
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
}

void PushRecent(std::vector<std::filesystem::path>& list, const std::filesystem::path& entry)
{
    list.erase(std::remove(list.begin(), list.end(), entry), list.end());
    list.insert(list.begin(), entry);
    if (static_cast<int>(list.size()) > kMaxRecent)
    {
        list.resize(kMaxRecent);
    }
}

// -----------------------------------------------------------------------------
// 可执行文件同目录的项目
//
// 第一次启动、最近列表还是空的时候，Launcher 里如果一条都没有，人只能靠"浏览…"
// 从 ~/Documents 一路摸过去。构建树里 ZEditor 和 ZGameSmoke.zproject 就是挨着的，
// 顺手把它们列出来，空列表也能一键进编辑器。
// -----------------------------------------------------------------------------
std::filesystem::path ExecutablePath()
{
    char     raw[4096] = {};
    uint32_t size      = static_cast<uint32_t>(sizeof(raw));
    if (_NSGetExecutablePath(raw, &size) != 0)
    {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::absolute(std::filesystem::path(raw), ec);
    return ec ? std::filesystem::path() : exe;
}

std::vector<std::filesystem::path> ProjectsBesideExecutable()
{
    const std::filesystem::path exe = ExecutablePath();
    if (exe.empty())
    {
        return {};
    }

    std::vector<std::filesystem::path> out;
    std::error_code                    ec;
    for (std::filesystem::directory_iterator it(exe.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file() && it->path().extension() == ".zproject")
        {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// -----------------------------------------------------------------------------
// Dock 图标
//
// `MacOSApplication::SetupCocoaOnce()` 里也装一次图标，但那要等 START_SYSTEM()
// 之后才跑。而 Launcher 是**在此之前**就把 NSApp 拉起来并设成
// ActivationPolicyRegular 的 —— Dock 磁贴那一刻就出现了，装图标的代码还没跑，
// 于是选项目的这几十秒里 Dock 上是一个通用图标。ZEditor 是裸 Mach-O 而不是
// .app，系统没有任何东西可以自动补上它（Windows 那边是 `z_embed_app_icon()`
// 把 .ico 嵌进 exe）。
//
// 路径首选 ini 里的 BigIconFile（跟 ConfigManager 同一条规则，见
// ResolveEditorBigIconPath）；ini 不在就退回几个布局候选，其中
// `../Resources/` 是 .app bundle 的布局（Contents/MacOS → Contents/Resources）。
// -----------------------------------------------------------------------------
std::filesystem::path FindAppIconFile()
{
    const std::filesystem::path exe = ExecutablePath();
    if (exe.empty())
    {
        return {};
    }
    const std::filesystem::path exe_dir = exe.parent_path();

    std::vector<std::filesystem::path> candidates;

    const std::filesystem::path from_ini = ResolveEditorBigIconPath(exe_dir / "ZEditor.ini");
    if (!from_ini.empty())
    {
        candidates.push_back(from_ini);
    }
    candidates.push_back(exe_dir / "resource" / "ZEngineEditorBigIcon.png");
    candidates.push_back(exe_dir / ".." / "Resources" / "ZEngineEditorBigIcon.png");
    candidates.push_back(exe_dir / "ZEngineEditorBigIcon.png");

    std::error_code ec;
    for (const std::filesystem::path& candidate : candidates)
    {
        if (std::filesystem::exists(candidate, ec))
        {
            return candidate;
        }
    }
    return {};
}

void InstallDockIcon()
{
    // 从 .app 里启动时图标由 AppKit 按 Info.plist 的 CFBundleIconFile 自己装上，
    // 就是 MakeMacOSAppBundle.cmake 生成的那份 ZEditor.icns。这时候再手动
    // setApplicationIconImage: 只会拿同一张 PNG 盖掉它 —— 视觉上没区别，但那样
    // icns 到底有没有生效就永远不可证伪了（Dock 上看不出图标来自哪边）。所以
    // bundle 自己有图标时这里让位，只留一条日志当证据。
    @autoreleasepool
    {
        NSBundle* main_bundle = [NSBundle mainBundle];
        NSString* bundle_id   = [main_bundle bundleIdentifier];
        NSString* icon_name   = [main_bundle objectForInfoDictionaryKey:@"CFBundleIconFile"];
        if (bundle_id.length > 0 && icon_name.length > 0)
        {
            LOG_INFO(ZEditor,
                     "Launcher: dock icon comes from the app bundle '{}' (Info.plist CFBundleIconFile = {}).",
                     [bundle_id UTF8String],
                     [icon_name UTF8String]);
            return;
        }
    }

    const std::filesystem::path icon = FindAppIconFile();
    if (icon.empty())
    {
        // 不打日志会更好吗？不会 —— 这条只会落在日志文件里，而"Dock 上是通用
        // 图标"这种缺陷，靠人眼是看不出原因的。
        LOG_WARNING(ZEditor, "Launcher: no app icon found next to the executable; the Dock tile will be generic.");
        return;
    }

    @autoreleasepool
    {
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:icon.c_str()]];
        if (image == nil)
        {
            LOG_WARNING(ZEditor, "Launcher: app icon cannot be decoded: '{}'", icon.string());
            return;
        }
        [NSApp setApplicationIconImage:image];
        [image release];
        LOG_INFO(ZEditor, "Launcher: dock icon installed from '{}'", icon.string());
    }
}

// -----------------------------------------------------------------------------
// 新建项目
//
// 只写 .zproject。目录脚手架（Scripts/ Shaders/ Data/ tsconfig.json ...）刻意留给
// ProjectInfo::EnsureScriptsScaffold() —— 它在编辑器第一次打开项目时从
// Initialize() 里跑，留一份实现，以后脚手架变样也不用改这里。
// -----------------------------------------------------------------------------
std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

bool CreateNewProject(const std::filesystem::path& parent_dir,
                      const std::string&           name,
                      std::filesystem::path&       out_project_file)
{
    if (name.empty() || parent_dir.empty())
    {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path project_dir = parent_dir / name;
    std::filesystem::create_directories(project_dir, ec);
    if (ec)
    {
        return false;
    }

    const std::filesystem::path project_file = project_dir / (name + ".zproject");

    // 镜像 ProjectInfo::Transfer()：.zproject 就是序列化后的 ProjectInfo，
    // 这几个字段是它读回去时唯一看的东西。
    const std::string json = std::string("{\n") +
        "  \"name\": \"" + JsonEscape(name) + "\",\n" +
        "  \"scripts_dir\": \"Scripts\",\n" +
        "  \"intermediate_dir\": \"Intermediate\",\n" +
        "  \"shaders_dir\": \"Shaders\",\n" +
        "  \"data_dir\": \"Data\",\n" +
        "  \"textures_dir\": \"Textures\",\n" +
        "  \"models_dir\": \"Models\"\n" +
        "}\n";

    FILE* f = std::fopen(project_file.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(json.data(), 1, json.size(), f) == json.size();
    std::fclose(f);
    if (!ok)
    {
        return false;
    }

    out_project_file = project_file;
    return true;
}

// -----------------------------------------------------------------------------
// 套件
// -----------------------------------------------------------------------------
struct FSuiteEntry
{
    std::string id;
    std::string display_name;
    std::string description;
    bool        needs_project = false;
};

std::vector<FSuiteEntry> CollectSuites()
{
    std::vector<FSuiteEntry> out;
    const ZEditor::ToolSuiteDesc& level = ZEditor::LevelEditorSuiteDesc();
    out.push_back({level.id, level.display_name, level.description, level.needs_project});
    for (const ZEditor::IToolSuite* suite : ZEditor::AllToolSuites())
    {
        const ZEditor::ToolSuiteDesc& d = suite->Desc();
        out.push_back({d.id, d.display_name, d.description, d.needs_project});
    }
    return out;
}

// 关卡编辑器不是 IToolSuite（它要跑 START_SYSTEM() 那一整套），所以单独比一次。
bool SuiteNeedsProject(const std::vector<FSuiteEntry>& suites, const std::string& suite_id)
{
    for (const FSuiteEntry& entry : suites)
    {
        if (entry.id == suite_id)
        {
            return entry.needs_project;
        }
    }
    return true;  // 认不出就当它要项目 —— 少启动一个空壳，比多启动一个好
}

// Launcher 跑起来期间共享的状态。做成文件内全局而不是 ObjC 类的 ivar：控制器
// 里放 C++ 非 POD 成员在 ObjC++ 下虽然能编，但这类 ivar 的构造/析构时机跟
// NSObject 的生命周期纠缠在一起，而这个进程里本来就只可能有一个 Launcher。
struct FLauncherState
{
    std::vector<FSuiteEntry>           suites;
    std::vector<std::filesystem::path> recent;
    std::filesystem::path              list_path;
    LaunchChoice                       choice;
    bool                               committed = false;
};

FLauncherState& State()
{
    static FLauncherState state;
    return state;
}

// -----------------------------------------------------------------------------
// 双击 .zproject 送进来的那个文件，到底是不是项目文件
//
// 系统那边已经按 Info.plist 的 UTI 筛过一遍了，但 LaunchServices 也允许"用这个
// App 打开"任意文件（右键 → 打开方式），那时候什么扩展名都可能送到
// -application:openFile:。而 .zproject **没有 magic 字节**（ProjectInfo::
// LoadFromFile 只判 exists，然后直接丢给 YAML/JSON 解析器），喂一个别的文件进去
// 不会报错，只会加载出一篇空白 —— 所以这里必须自己认扩展名。
//
// 扩展名比大小写：macOS 默认文件系统不区分大小写，但用户手改的后缀未必是小写。
// -----------------------------------------------------------------------------
bool IsZProjectFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    for (char& c : extension)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (extension != ".zproject")
    {
        return false;
    }

    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}  // namespace
}  // namespace ZEditorLauncher

// -----------------------------------------------------------------------------
// AppKit 窗口
// -----------------------------------------------------------------------------
// NSApplicationDelegate 是**双击 .zproject 那条路唯一的入口**：LaunchServices 不把
// 文件路径放进 argv（argv 里只有 -psn_0_xxxxx），而是等 App 就绪之后发一个
// kAEOpenDocuments（odoc）Apple Event，NSApplication 再转给 delegate 的
// -application:openFile:。没有 delegate，那个事件就被静默丢掉 —— 症状是"双击
// .zproject 没反应"，既不报错也不弹窗。
@interface ZLauncherController : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource>
{
@public
    NSWindow*      _window;
    NSPopUpButton* _suitePopup;
    NSTextField*   _hintLabel;
    NSTableView*   _table;
    NSTextField*   _nameField;
    NSTextField*   _dirField;
    NSTextField*   _statusLabel;
}

- (BOOL)runLauncher;
- (void)suiteChanged:(id)sender;
- (void)openSelected:(id)sender;
- (void)browseForProject:(id)sender;
- (void)createAndOpen:(id)sender;
- (void)quit:(id)sender;
- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename;
@end

@implementation ZLauncherController

// -----------------------------------------------------------------------------
// 提交选择并让主循环退出
//
// `[NSApp stop:]` 只在"下一次拿到控制权"时才结束 -run，光调它不保证立刻返回；
// 补一个自定义事件把 run loop 唤醒是这套组合的标准写法，缺了它会出现"点了没反应"。
// -----------------------------------------------------------------------------
- (void)finishWithChoice:(const std::filesystem::path&)project_file
{
    namespace ZL = ZEditorLauncher;
    ZL::FLauncherState& state = ZL::State();

    const NSInteger index = [_suitePopup indexOfSelectedItem];
    if (index < 0 || index >= static_cast<NSInteger>(state.suites.size()))
    {
        return;
    }

    state.choice.suite_id     = state.suites[static_cast<size_t>(index)].id;
    state.choice.project_file = project_file;
    state.committed           = true;

    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0]
             atStart:NO];
}

- (void)setStatus:(NSString*)message
{
    _statusLabel.stringValue = message;
}

- (void)suiteChanged:(id)sender
{
    namespace ZL = ZEditorLauncher;
    const ZL::FLauncherState& state = ZL::State();

    const NSInteger index = [_suitePopup indexOfSelectedItem];
    if (index < 0 || index >= static_cast<NSInteger>(state.suites.size()))
    {
        return;
    }

    const ZL::FSuiteEntry& entry = state.suites[static_cast<size_t>(index)];
    _hintLabel.stringValue       = [NSString stringWithUTF8String:entry.description.c_str()];

    // 不需要项目的套件（烘焙器 / 查看器）选完就走：再让人去挑一个 .zproject
    // 只会让他以为这工具跟项目有关。跟 Win32 版同一条规则。
    if (!entry.needs_project)
    {
        [self finishWithChoice:std::filesystem::path()];
    }
}

- (void)openSelected:(id)sender
{
    namespace ZL = ZEditorLauncher;
    const ZL::FLauncherState& state = ZL::State();

    const NSInteger row = [_table selectedRow];
    if (row < 0 || row >= static_cast<NSInteger>(state.recent.size()))
    {
        [self setStatus:@"先在列表里选一个项目，或用“浏览…”找一个。"];
        return;
    }
    [self finishWithChoice:state.recent[static_cast<size_t>(row)]];
}

- (void)browseForProject:(id)sender
{
    @autoreleasepool
    {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles          = YES;
        panel.canChooseDirectories    = NO;
        panel.allowsMultipleSelection = NO;
        panel.message                 = @"选择一个 ZEngine 项目";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // allowedFileTypes 在 10.13 起被 allowedContentTypes 取代，但后者要求
        // .zproject 是一个已声明的 UTType（要有 app bundle 的 Info.plist），
        // 而 ZEditor 现在是裸可执行文件。扩展名过滤在这里够用。
        panel.allowedFileTypes = @[ @"zproject" ];
#pragma clang diagnostic pop

        if ([panel runModal] != NSModalResponseOK)
        {
            return;
        }

        NSURL* url = panel.URL;
        if (url == nil || url.path == nil)
        {
            return;
        }
        [self finishWithChoice:std::filesystem::path(url.path.UTF8String)];
    }
}

- (void)createAndOpen:(id)sender
{
    namespace ZL = ZEditorLauncher;

    const std::string name = _nameField.stringValue.UTF8String ? _nameField.stringValue.UTF8String : "";
    if (name.empty())
    {
        [self setStatus:@"请填写项目名称"];
        return;
    }

    const char* dir_utf8 = _dirField.stringValue.UTF8String;
    if (dir_utf8 == nullptr || dir_utf8[0] == '\0')
    {
        [self setStatus:@"请填写项目位置"];
        return;
    }

    std::filesystem::path created;
    if (!ZL::CreateNewProject(std::filesystem::path(dir_utf8), name, created))
    {
        [self setStatus:@"创建失败，请检查位置是否可写、名称是否合法"];
        return;
    }

    [self finishWithChoice:created];
}

- (void)quit:(id)sender
{
    [NSApp terminate:nil];
}

// -----------------------------------------------------------------------------
// 双击 .zproject（LaunchServices 的 kAEOpenDocuments）
//
// 系统不会把路径放进 argv —— 双击启动时 argv 里只有 `-psn_0_xxxxx`（那个参数会被
// CommandParser 自动注册成 Flag 后忽略，不会让进程退出）。路径是等 App 就绪之后
// 以 Apple Event 送过来的，NSApplication 转给 delegate 的这个方法。
//
// 不能只记日志：拿到路径必须真的 `finishWithChoice:` 提交，否则 Launcher 会继续
// 等在那里，用户看到的是"双击了 zproject，然后弹了个选项目的窗口"—— 等于没做。
// -----------------------------------------------------------------------------
- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename
{
    namespace ZL = ZEditorLauncher;
    if (filename == nil)
    {
        return NO;
    }

    const std::filesystem::path path(filename.fileSystemRepresentation);
    if (!ZL::IsZProjectFile(path))
    {
        LOG_WARNING(ZEditor, "Launcher: ignored '{}' -- not a .zproject on disk.", path.string());
        return NO;
    }

    LOG_INFO(ZEditor, "Launcher: opening document '{}' (from LaunchServices).", path.string());
    [self finishWithChoice:path];
    return YES;
}

- (void)windowWillClose:(NSNotification*)notification
{
    // 关窗口 == 取消。不打日志、不报错：这是人主动放弃，不是故障。
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0]
             atStart:NO];
}

// ---- NSTableViewDataSource ----
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return static_cast<NSInteger>(ZEditorLauncher::State().recent.size());
}

- (id)tableView:(NSTableView*)tableView
    objectValueForTableColumn:(NSTableColumn*)tableColumn
                          row:(NSInteger)row
{
    const ZEditorLauncher::FLauncherState& state = ZEditorLauncher::State();
    if (row < 0 || row >= static_cast<NSInteger>(state.recent.size()))
    {
        return nil;
    }

    const std::filesystem::path& project = state.recent[static_cast<size_t>(row)];
    const bool                   is_path = [tableColumn.identifier isEqualToString:@"path"];
    const std::string            value   = is_path ? project.parent_path().string() : project.stem().string();
    return [NSString stringWithUTF8String:value.c_str()];
}

- (BOOL)runLauncher
{
    namespace ZL = ZEditorLauncher;
    ZL::FLauncherState& state = ZL::State();

    @autoreleasepool
    {
        NSApplication* app = [NSApplication sharedApplication];
        // 裸可执行文件默认是 ActivationPolicyProhibited：窗口能显示但拿不到键盘
        // 焦点、不进 Dock、没有菜单栏（于是文本框里 Cmd-V 也没用）。Regular 才
        // 是"一个正经 App"。
        //
        // ⚠️ 一设 Regular，Dock 磁贴立刻出现 —— 图标必须**在这一行之后、
        // [app run] 之前**装上，否则选项目的这几十秒里 Dock 上是个通用图标。
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        ZL::InstallDockIcon();

        // 必须装在 finishLaunching 之前：LaunchServices 是在 App "就绪"那一刻才把
        // odoc（kAEOpenDocuments）发过来的，而派发对象是 delegate。装晚了事件就
        // 已经过去了。

        [app setDelegate:self];


        const NSRect frame = NSMakeRect(0, 0, 780.0, 660.0);
        NSWindow*    window =
            [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                   NSWindowStyleMaskMiniaturizable)
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.title                = @"ZEngine Launcher";
        window.delegate             = self;
        window.releasedWhenClosed   = NO;
        _window                     = window;

        NSView* content = window.contentView;

        const CGFloat kMargin     = 24.0;
        const CGFloat inner_width = frame.size.width - kMargin * 2.0;
        CGFloat       cursor_y    = frame.size.height - kMargin;

        auto place = [&](NSView* view, CGFloat height, CGFloat gap_before)
        {
            cursor_y -= gap_before;
            view.frame = NSMakeRect(kMargin, cursor_y - height, inner_width, height);
            [content addSubview:view];
            cursor_y -= height;
        };

        auto make_label = [](NSString* text, CGFloat size, BOOL bold, NSColor* colour) -> NSTextField*
        {
            NSTextField* label  = [[NSTextField alloc] initWithFrame:NSZeroRect];
            label.stringValue   = text;
            label.bezeled       = NO;
            label.drawsBackground = NO;
            label.editable      = NO;
            label.selectable    = NO;
            label.font          = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
            label.textColor     = colour;
            return label;
        };

        NSColor* const kMuted  = [NSColor colorWithCalibratedWhite:0.62 alpha:1.0];
        NSColor* const kAccent = [NSColor colorWithCalibratedRed:0.55 green:0.68 blue:0.92 alpha:1.0];
        NSColor* const kError  = [NSColor colorWithCalibratedRed:0.93 green:0.52 blue:0.48 alpha:1.0];

        place(make_label(@"ZEngine Launcher", 26.0, YES, [NSColor labelColor]), 34.0, 0.0);
        place(make_label(@"选择工具套件，然后选一个项目", 13.0, NO, kAccent), 20.0, 0.0);

        // ---- 套件 ----
        place(make_label(@"工具套件", 13.0, YES, kMuted), 18.0, 16.0);

        _suitePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (const ZL::FSuiteEntry& entry : state.suites)
        {
            const std::string label = (entry.needs_project ? "" : "[直接启动] ") + entry.display_name;
            [_suitePopup addItemWithTitle:[NSString stringWithUTF8String:label.c_str()]];
        }
        _suitePopup.target = self;
        _suitePopup.action = @selector(suiteChanged:);
        place(_suitePopup, 26.0, 4.0);

        _hintLabel                          = make_label(@"", 12.0, NO, kMuted);
        _hintLabel.maximumNumberOfLines     = 2;
        _hintLabel.lineBreakMode            = NSLineBreakByWordWrapping;
        place(_hintLabel, 30.0, 4.0);

        // ---- 最近项目 ----
        place(make_label(@"最近项目", 13.0, YES, kMuted), 18.0, 16.0);

        NSScrollView* scroll      = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        scroll.hasVerticalScroller = YES;
        scroll.borderType          = NSBezelBorder;

        _table                             = [[NSTableView alloc] initWithFrame:NSZeroRect];
        _table.dataSource                  = self;
        _table.rowHeight                   = 22.0;
        _table.doubleAction                = @selector(openSelected:);
        _table.target                      = self;
        _table.columnAutoresizingStyle     = NSTableViewUniformColumnAutoresizingStyle;

        NSTableColumn* name_column = [[NSTableColumn alloc] initWithIdentifier:@"name"];
        name_column.title          = @"项目";
        name_column.width          = 240.0;
        [_table addTableColumn:name_column];

        NSTableColumn* path_column = [[NSTableColumn alloc] initWithIdentifier:@"path"];
        path_column.title          = @"路径";
        path_column.width          = inner_width - 240.0;
        [_table addTableColumn:path_column];

        scroll.documentView = _table;
        place(scroll, 170.0, 4.0);

        if (state.recent.empty())
        {
            place(make_label(@"最近列表还是空的 —— 用下面的按钮打开或新建一个项目。", 12.0, NO, kMuted),
                  18.0,
                  2.0);
        }
        else
        {
            [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        }

        // ---- 打开 / 浏览 ----
        NSButton* open_button =
            [self makeButton:@"打开" action:@selector(openSelected:) width:110.0 height:30.0];
        NSButton* browse_button =
            [self makeButton:@"浏览…" action:@selector(browseForProject:) width:110.0 height:30.0];
        place([self makeRow:@[ open_button, browse_button ] width:inner_width height:30.0], 30.0, 8.0);

        // ---- 新建项目 ----
        place(make_label(@"新建项目", 15.0, YES, [NSColor labelColor]), 20.0, 16.0);

        _nameField                    = [self makeTextField:@"项目名称" value:@""];
        place(_nameField, 24.0, 4.0);

        const std::string default_dir = ZL::DefaultProjectsDir().string();
        _dirField                     = [self makeTextField:@"位置"
                                                     value:[NSString stringWithUTF8String:default_dir.c_str()]];
        place(_dirField, 24.0, 6.0);

        NSButton* create_button =
            [self makeButton:@"创建并打开" action:@selector(createAndOpen:) width:150.0 height:30.0];
        place(create_button, 30.0, 8.0);

        _statusLabel       = make_label(@"", 12.0, NO, kError);
        place(_statusLabel, 18.0, 6.0);

        NSButton* quit_button = [self makeButton:@"退出" action:@selector(quit:) width:110.0 height:28.0];
        place(quit_button, 28.0, 12.0);

        // 初始套件描述
        [self suiteChanged:_suitePopup];

        // -------------------------------------------------------------------
        // 双击 .zproject 时别把 Launcher 窗口闪出来
        //
        // LaunchServices 要等 App "就绪"才发 odoc，而"就绪"就是 finishLaunching。
        // 所以这里先 finishLaunching 再手动泵一小段：odoc 到了就
        // -application:openFile: → finishWithChoice: → state.committed，直接返回，
        // 窗口一次都不用显示。收不到（正常双击 ZEditor.app 的情况）就退回老路：
        // 显示窗口 + [app run]。
        //
        // 代价：这种情况下窗口晚 0.5 秒出现。换来的是"双击 .zproject → 直接进
        // 编辑器"，而不是"双击 .zproject → 闪一个选项目的窗口 → 再进编辑器"。
        // -------------------------------------------------------------------
        [app finishLaunching];
        NSDate* const open_deadline = [NSDate dateWithTimeIntervalSinceNow:0.5];
        while (!state.committed && [open_deadline timeIntervalSinceNow] > 0.0)
        {
            @autoreleasepool
            {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:open_deadline
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (event == nil)
                {
                    break;
                }
                [NSApp sendEvent:event];
            }
        }

        if (state.committed)
        {
            return YES;
        }

        [window center];
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }

    return state.committed ? YES : NO;
}

- (NSButton*)makeButton:(NSString*)title action:(SEL)action width:(CGFloat)width height:(CGFloat)height
{
    NSButton* button = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    button.title     = title;
    button.bezelStyle = NSBezelStyleRounded;
    button.target     = self;
    button.action     = action;
    button.font       = [NSFont systemFontOfSize:13.0];
    return button;
}

- (NSView*)makeRow:(NSArray<NSButton*>*)buttons width:(CGFloat)width height:(CGFloat)height
{
    NSView* row = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    CGFloat x   = 0.0;
    for (NSButton* button in buttons)
    {
        const CGFloat w = button.frame.size.width;
        button.frame    = NSMakeRect(x, 0.0, w, height);
        [row addSubview:button];
        x += w + 10.0;
    }
    return row;
}

- (NSTextField*)makeTextField:(NSString*)placeholder value:(NSString*)value
{
    NSTextField* field  = [[NSTextField alloc] initWithFrame:NSZeroRect];
    field.placeholderString = placeholder;
    field.stringValue   = value;
    field.bezeled       = YES;
    field.bezelStyle    = NSTextFieldSquareBezel;
    field.drawsBackground = YES;
    field.editable      = YES;
    field.selectable    = YES;
    field.font          = [NSFont systemFontOfSize:13.0];
    return field;
}

// 没有菜单栏的话，文本框里 Cmd-C / Cmd-V / Cmd-A 全都不会走 responder chain
// （第一响应者是 field editor，动作要靠菜单项转发）。
- (void)installMainMenu
{
    NSMenu* main_menu = [[NSMenu alloc] initWithTitle:@"MainMenu"];

    NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:@"ZEditor" action:nil keyEquivalent:@""];
    NSMenu*     app_menu = [[NSMenu alloc] initWithTitle:@"ZEditor"];
    NSMenuItem* quit_item =
        [[NSMenuItem alloc] initWithTitle:@"退出 ZEditor" action:@selector(terminate:) keyEquivalent:@"q"];
    [app_menu addItem:quit_item];
    [app_item setSubmenu:app_menu];
    [main_menu addItem:app_item];

    NSMenuItem* edit_item = [[NSMenuItem alloc] initWithTitle:@"编辑" action:nil keyEquivalent:@""];
    NSMenu*     edit_menu = [[NSMenu alloc] initWithTitle:@"编辑"];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"]];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"重做"
                                                  action:@selector(redo:)
                                           keyEquivalent:@"Z"]];
    [edit_menu addItem:[NSMenuItem separatorItem]];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"]];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"拷贝" action:@selector(copy:) keyEquivalent:@"c"]];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"]];
    [edit_menu addItem:[[NSMenuItem alloc] initWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"]];
    [edit_item setSubmenu:edit_menu];
    [main_menu addItem:edit_item];

    [NSApp setMainMenu:main_menu];
}

@end

namespace ZEditorLauncher
{

bool Run(LaunchChoice& out_choice)
{
    FLauncherState& state = State();

    state.suites    = CollectSuites();
    state.list_path = RecentListPath();
    state.recent    = LoadRecent(state.list_path);
    state.choice    = LaunchChoice();
    state.committed = false;

    // 最近列表为空时补上可执行文件同目录的项目：第一次启动（或从没手动打开过
    // 项目）也能一键进编辑器，不用先摸一遍 Finder。
    if (state.recent.empty())
    {
        for (std::filesystem::path& candidate : ProjectsBesideExecutable())
        {
            PushRecent(state.recent, std::move(candidate));
        }
    }

    ZLauncherController* controller = [[ZLauncherController alloc] init];
    const BOOL ok = [controller runLauncher];

    if (!ok || !state.committed)
    {
        return false;
    }

    if (SuiteNeedsProject(state.suites, state.choice.suite_id))
    {
        if (state.choice.project_file.empty())
        {
            return false;
        }
        PushRecent(state.recent, state.choice.project_file);
        SaveRecent(state.list_path, state.recent);
    }

    out_choice = state.choice;
    return true;
}

void RememberProject(const std::filesystem::path& project_file)
{
    if (project_file.empty())
    {
        return;
    }

    const std::filesystem::path list_path = RecentListPath();
    if (list_path.empty())
    {
        return;
    }

    std::vector<std::filesystem::path> recent = LoadRecent(list_path);
    PushRecent(recent, project_file);
    SaveRecent(list_path, recent);
}

}  // namespace ZEditorLauncher

#endif  // __APPLE__
