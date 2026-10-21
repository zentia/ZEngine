#include "Editor/Launcher/ZEditorLauncher.h"
#include "Editor/ToolSuite/ToolSuite.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <KnownFolders.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <windows.h>

#include "ZSlate/App/SlateStandaloneApp.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/SWidget.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace ZEditorLauncher
{
namespace
{
    constexpr int kMaxRecent = 10;

    // -------------------------------------------------------------------------
    // Encoding. The recent list holds paths that may contain non-ASCII, and it is
    // stored as UTF-8 so the file stays readable in any editor. std::filesystem
    // talks UTF-16 on Windows, hence the explicit conversion: relying on the
    // process locale here would silently corrupt paths on a non-UTF-8 codepage.
    // -------------------------------------------------------------------------
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        const int needed =
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
        return out;
    }

    std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty()) return {};
        const int needed = WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || raw == nullptr) return {};
        std::filesystem::path folder = raw;
        CoTaskMemFree(raw);
        return folder;
    }

    std::filesystem::path RecentListPath()
    {
        const std::filesystem::path base = KnownFolder(FOLDERID_LocalAppData);
        if (base.empty()) return {};
        return base / "ZEngine" / "ZEditor" / "recent_projects.txt";
    }

    std::filesystem::path DefaultProjectsDir()
    {
        const std::filesystem::path documents = KnownFolder(FOLDERID_Documents);
        if (documents.empty()) return {};
        return documents / "ZEngine";
    }

    std::vector<std::filesystem::path> LoadRecent(const std::filesystem::path& list_path)
    {
        std::vector<std::filesystem::path> out;
        FILE* f = _wfopen(list_path.c_str(), L"rb");
        if (f == nullptr) return out;

        std::string all;
        char buf[4096];
        size_t read = 0;
        while ((read = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, read);
        fclose(f);

        size_t start = 0;
        while (start < all.size())
        {
            size_t end = all.find('\n', start);
            if (end == std::string::npos) end = all.size();
            std::string line = all.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty())
            {
                // Drop entries whose project was moved or deleted since.
                std::filesystem::path candidate(Utf8ToWide(line));
                if (std::filesystem::exists(candidate)) out.push_back(std::move(candidate));
            }
            start = end + 1;
        }
        return out;
    }

    void SaveRecent(const std::filesystem::path& list_path, const std::vector<std::filesystem::path>& list)
    {
        std::error_code ec;
        std::filesystem::create_directories(list_path.parent_path(), ec);
        FILE* f = _wfopen(list_path.c_str(), L"wb");
        if (f == nullptr) return;
        for (const auto& entry : list)
        {
            const std::string line = WideToUtf8(entry.wstring()) + "\n";
            fwrite(line.data(), 1, line.size(), f);
        }
        fclose(f);
    }

    void PushRecent(std::vector<std::filesystem::path>& list, const std::filesystem::path& entry)
    {
        list.erase(std::remove(list.begin(), list.end(), entry), list.end());
        list.insert(list.begin(), entry);
        if (static_cast<int>(list.size()) > kMaxRecent) list.resize(kMaxRecent);
    }

    // -------------------------------------------------------------------------
    // Projects
    // -------------------------------------------------------------------------
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

    /// Creates <parent>/<name>/ plus <parent>/<name>/<name>.zproject.
    ///
    /// Only the project file is written. The folder scaffolding (Scripts/,
    /// Shaders/, Data/, tsconfig.json, ...) is deliberately left to
    /// ProjectInfo::EnsureScriptsScaffold(), which runs from Initialize() the
    /// first time the editor opens the project -- one implementation, and it
    /// stays correct if that scaffold grows.
    bool CreateNewProject(const std::filesystem::path& parent_dir,
                          const std::string&           name,
                          std::filesystem::path&       out_project_file)
    {
        if (name.empty() || parent_dir.empty()) return false;

        std::error_code ec;
        const std::filesystem::path project_dir = parent_dir / Utf8ToWide(name);
        std::filesystem::create_directories(project_dir, ec);
        if (ec) return false;

        const std::filesystem::path project_file = project_dir / (Utf8ToWide(name) + L".zproject");

        // Mirrors ProjectInfo::Transfer(): the .zproject is just the serialised
        // ProjectInfo, and those are the fields it reads back.
        const std::string json = std::string("{\n") +
            "  \"name\": \"" + JsonEscape(name) + "\",\n" +
            "  \"scripts_dir\": \"Scripts\",\n" +
            "  \"intermediate_dir\": \"Intermediate\",\n" +
            "  \"shaders_dir\": \"Shaders\",\n" +
            "  \"data_dir\": \"Data\",\n" +
            "  \"textures_dir\": \"Textures\",\n" +
            "  \"models_dir\": \"Models\"\n" +
            "}\n";

        FILE* f = _wfopen(project_file.c_str(), L"wb");
        if (f == nullptr) return false;
        const bool ok = fwrite(json.data(), 1, json.size(), f) == json.size();
        fclose(f);
        if (!ok) return false;

        out_project_file = project_file;
        return true;
    }

    std::filesystem::path BrowseForProject(HWND owner)
    {
        wchar_t buffer[4096] = {};
        OPENFILENAMEW ofn {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = owner;
        ofn.lpstrFile   = buffer;
        ofn.nMaxFile    = static_cast<DWORD>(std::size(buffer));
        ofn.lpstrFilter = L"ZEngine 项目 (*.zproject)\0*.zproject\0所有文件 (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle   = L"打开 ZEngine 项目";
        ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING;

        if (!GetOpenFileNameW(&ofn)) return {};
        return std::filesystem::path(buffer);
    }

    // -------------------------------------------------------------------------
    // File association
    //
    // Lets a double clicked .zproject open the editor directly, which is the
    // whole point for someone who does not know about -p.
    //
    // Written under HKCU\Software\Classes, not HKCR: HKCU needs no elevation and
    // Windows merges it into the effective HKCR view anyway.
    // -------------------------------------------------------------------------
    constexpr const wchar_t* kProgId = L"ZEngine.Project";

    std::wstring ExePath()
    {
        wchar_t buffer[4096] = {};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
        return length != 0 ? std::wstring(buffer) : std::wstring{};
    }

    bool SetKeyString(HKEY root, const std::wstring& sub_key, const std::wstring& data)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(root, sub_key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS)
        {
            return false;
        }
        const bool ok =
            RegSetValueExW(key,
                           nullptr,
                           0,
                           REG_SZ,
                           reinterpret_cast<const BYTE*>(data.c_str()),
                           static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        RegCloseKey(key);
        return ok;
    }

    bool RegisterFileAssociation()
    {
        const std::wstring exe = ExePath();
        if (exe.empty()) return false;

        const std::wstring prog_key = std::wstring(L"Software\\Classes\\") + kProgId;
        bool ok = true;
        ok &= SetKeyString(HKEY_CURRENT_USER, L"Software\\Classes\\.zproject", kProgId);
        ok &= SetKeyString(HKEY_CURRENT_USER, prog_key, L"ZEngine 项目");
        // DefaultIcon: ",0" picks the first (largest) icon baked into the exe.
        ok &= SetKeyString(HKEY_CURRENT_USER, prog_key + L"\\DefaultIcon", L"\"" + exe + L"\",0");
        ok &= SetKeyString(HKEY_CURRENT_USER, prog_key + L"\\shell\\open\\command", L"\"" + exe + L"\" -p \"%1\"");

        // Explorer caches associations; without this the new icon and open verb
        // do not appear until it happens to re-read the registry.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
        return ok;
    }

    bool IsFileAssociationRegistered()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.zproject", 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return false;
        }
        wchar_t value[256] = {};
        DWORD   size       = sizeof(value);
        const LSTATUS status =
            RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        return status == ERROR_SUCCESS && std::wstring(value) == kProgId;
    }

    // -------------------------------------------------------------------------
    // UI
    // -------------------------------------------------------------------------
    std::shared_ptr<ZSlate::STextBlock> MakeLabel(const std::string& text,
                                                  float              size,
                                                  const ZSlate::UIColor& colour)
    {
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text     = text;
        label->FontSize = size;
        label->Color    = colour;
        return label;
    }

    std::shared_ptr<ZSlate::SButton> MakeButton(const std::string&      text,
                                                std::function<void()>   on_clicked)
    {
        auto button = std::make_shared<ZSlate::SButton>();
        button->SetContent(MakeLabel(text, 15.0f, ZSlate::Colors::White));
        button->OnClicked = std::move(on_clicked);
        return button;
    }

    // -------------------------------------------------------------------------
    // 套件列表
    //
    // 关卡编辑器排在第一个并且默认选中：绝大多数启动就是要它，让它变成"再点一次
    // 项目"而不是"先点套件再点项目"。其它套件是 `ZEditor::AllToolSuites()`
    // 里自注册的那些 —— 加一个套件不用改这里。
    // -------------------------------------------------------------------------
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

    std::shared_ptr<ZSlate::SWidget> MakeProjectButton(ZSlate::FSlateStandaloneApp&   app,
                                                       const std::filesystem::path&  project_file,
                                                       const std::vector<FSuiteEntry>& suites,
                                                       const std::shared_ptr<int>&    selected,
                                                       LaunchChoice&                  choice)
    {
        auto button = std::make_shared<ZSlate::SButton>();
        auto body   = std::make_shared<ZSlate::SVerticalBox>();

        body->AddSlot(MakeLabel(WideToUtf8(project_file.stem().wstring()), 15.0f, ZSlate::Colors::White));
        body->AddSlot(MakeLabel(WideToUtf8(project_file.parent_path().wstring()),
                                11.0f,
                                ZSlate::UIColor(0.52f, 0.55f, 0.62f, 1.0f)));

        button->SetContent(body);
        button->OnClicked = [&app, &choice, &suites, selected, project_file]()
        {
            choice.suite_id     = suites[*selected].id;
            choice.project_file = project_file;
            app.RequestExit();
        };
        return button;
    }

    std::shared_ptr<ZSlate::SWidget> BuildRoot(ZSlate::FSlateStandaloneApp&             app,
                                               const std::vector<std::filesystem::path>& recent,
                                               LaunchChoice&                            choice)
    {
        using namespace ZSlate;

        const std::vector<FSuiteEntry> suites = CollectSuites();
        // 按钮的 lambda 要在 app.Run() 期间改写它，而 BuildRoot 早就返回了 ——
        // 必须是共享的，引用捕获栈上的 int 会在第一次点击时就悬空。
        const auto selected = std::make_shared<int>(0);

        auto root = std::make_shared<SVerticalBox>();
        const UIColor kMuted(0.60f, 0.63f, 0.70f, 1.0f);

        root->AddSlot(MakeLabel("ZEngine Launcher", 26.0f, Colors::White));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 4)));
        root->AddSlot(MakeLabel("选择工具套件，然后选一个项目", 14.0f, UIColor(0.60f, 0.72f, 0.92f, 1.0f)));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 14)));

        root->AddSlot(MakeLabel("工具套件", 15.0f, UIColor(0.78f, 0.80f, 0.86f, 1.0f)));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 6)));

        auto suite_hint = MakeLabel("", 12.0f, kMuted);
        const auto refresh_hint = [suites, selected, suite_hint]()
        {
            suite_hint->Text = suites[*selected].description;
        };
        refresh_hint();

        for (size_t i = 0; i < suites.size(); ++i)
        {
            const std::string label = (suites[i].needs_project ? "" : "[直接启动] ") + suites[i].display_name;
            auto              button = std::make_shared<SButton>();
            button->SetContent(MakeLabel(label, 14.0f, Colors::White));
            button->OnClicked = [&app, &choice, suites, selected, suite_hint, refresh_hint, i]()
            {
                *selected = static_cast<int>(i);
                refresh_hint();
                // 不需要项目的套件（烘焙器 / 查看器）选完就走：再让用户去挑一个
                // .zproject 只会让他以为这工具跟项目有关。
                if (!suites[i].needs_project)
                {
                    choice.suite_id = suites[i].id;
                    app.RequestExit();
                }
            };
            root->AddSlot(button);
            root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 4)));
        }
        root->AddSlot(suite_hint);
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 18)));

        if (!recent.empty())
        {
            root->AddSlot(MakeLabel("最近项目", 15.0f, UIColor(0.78f, 0.80f, 0.86f, 1.0f)));
            for (const auto& project_file : recent)
            {
                root->AddSlot(MakeProjectButton(app, project_file, suites, selected, choice));
                root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 4)));
            }
        }
        else
        {
            root->AddSlot(MakeLabel("还没有项目，用下面的按钮打开或新建一个。", 14.0f, kMuted));
        }

        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 16)));
        root->AddSlot(MakeButton("打开项目…",
                                 [&app, &choice, suites, selected]()
                                 {
                                     const std::filesystem::path chosen = BrowseForProject(app.GetHwnd());
                                     if (chosen.empty()) return;
                                     choice.suite_id     = suites[*selected].id;
                                     choice.project_file = chosen;
                                     app.RequestExit();
                                 }));

        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 22)));
        root->AddSlot(MakeLabel("新建项目", 17.0f, Colors::White));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 8)));

        auto status = MakeLabel("", 12.0f, UIColor(0.92f, 0.52f, 0.48f, 1.0f));

        auto name_box = std::make_shared<SEditableTextBox>();
        name_box->HintText = "项目名称";
        name_box->MinWidth = 360.0f;

        auto dir_box = std::make_shared<SEditableTextBox>();
        dir_box->SetText(WideToUtf8(DefaultProjectsDir().wstring()));
        dir_box->MinWidth = 360.0f;

        root->AddSlot(MakeLabel("名称", 13.0f, kMuted));
        root->AddSlot(name_box);
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 6)));
        root->AddSlot(MakeLabel("位置", 13.0f, kMuted));
        root->AddSlot(dir_box);
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 10)));

        root->AddSlot(MakeButton("创建并打开",
                                 [&app, &choice, suites, selected, name_box, dir_box, status]()
                                 {
                                     const std::string name = name_box->GetText();
                                     if (name.empty())
                                     {
                                         status->Text = "请填写项目名称";
                                         return;
                                     }

                                     const std::filesystem::path parent(Utf8ToWide(dir_box->GetText()));
                                     std::filesystem::path created;
                                     if (!CreateNewProject(parent, name, created))
                                     {
                                         status->Text = "创建失败，请检查位置是否可写";
                                         return;
                                     }

                                     choice.suite_id     = suites[*selected].id;
                                     choice.project_file = created;
                                     app.RequestExit();
                                 }));
        root->AddSlot(status);

        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 20)));
        root->AddSlot(MakeLabel("文件关联", 15.0f, UIColor(0.78f, 0.80f, 0.86f, 1.0f)));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 6)));

        auto assoc_label = MakeLabel(IsFileAssociationRegistered()
                                         ? "已关联 .zproject 文件，双击项目文件即可打开"
                                         : "关联 .zproject 文件（之后双击项目文件即可打开）",
                                     13.0f,
                                     kMuted);

        auto assoc_button = std::make_shared<SButton>();
        assoc_button->SetContent(assoc_label);
        assoc_button->OnClicked = [assoc_label]()
        {
            assoc_label->Text = RegisterFileAssociation()
                                    ? "已关联 .zproject 文件，双击项目文件即可打开"
                                    : "关联失败：无法写入注册表";
        };
        root->AddSlot(assoc_button);

        return root;
    }
}  // namespace

// 套件是否需要项目。关卡编辑器不是 IToolSuite（它要跑 START_SYSTEM() 那一整套），
// 所以它只在这里有个 desc，查找时要单独比一次。
bool SuiteNeedsProject(const std::string& suite_id)
{
    if (suite_id == ZEditor::LevelEditorSuiteDesc().id)
    {
        return ZEditor::LevelEditorSuiteDesc().needs_project;
    }
    if (const ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(suite_id))
    {
        return suite->Desc().needs_project;
    }
    return true;  // 认不出就当它要项目 —— 少启动一个空壳，比多启动一个好
}

bool Run(LaunchChoice& out_choice)
{
    const std::filesystem::path list_path   = RecentListPath();
    std::vector<std::filesystem::path> recent = LoadRecent(list_path);

    LaunchChoice choice;

    ZSlate::FStandaloneAppDesc desc;
    desc.Title  = "ZEngine Launcher";
    desc.Width  = 780.0f;
    desc.Height = 640.0f;
    desc.Font   = ZSlate::Win32::ESystemFont::Sans;
#ifdef IDI_ICON1
    desc.LargeIconResId = IDI_ICON1;
    desc.SmallIconResId = IDI_ICON1;
#endif

    ZSlate::FSlateStandaloneApp app;
    if (!app.Initialize(desc))
    {
        // Nothing can be printed: this is a GUI-subsystem process. A launch that
        // produces no window at all is indistinguishable from a crash, so say so.
        MessageBoxW(nullptr,
                    L"ZEditor 需要一个项目才能启动。\n\n"
                    L"项目选择窗口无法创建（ZSlate 初始化失败）。\n"
                    L"请用命令行指定项目：ZEditor -p <项目.zproject>",
                    L"ZEditor",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    app.SetRootContent(BuildRoot(app, recent, choice));
    app.Run();
    app.Shutdown();

    if (choice.suite_id.empty()) return false;

    const bool needs_project = SuiteNeedsProject(choice.suite_id);
    if (needs_project && choice.project_file.empty()) return false;

    if (needs_project && !choice.project_file.empty())
    {
        PushRecent(recent, choice.project_file);
        SaveRecent(list_path, recent);
    }

    out_choice = choice;
    return true;
}

void RememberProject(const std::filesystem::path& project_file)
{
    if (project_file.empty()) return;

    const std::filesystem::path list_path = RecentListPath();
    if (list_path.empty()) return;

    std::vector<std::filesystem::path> recent = LoadRecent(list_path);
    PushRecent(recent, project_file);
    SaveRecent(list_path, recent);
}

}  // namespace ZEditorLauncher

#endif  // _WIN32
