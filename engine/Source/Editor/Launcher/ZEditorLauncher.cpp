#include "Editor/Launcher/ZEditorLauncher.h"

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

    std::shared_ptr<ZSlate::SWidget> MakeProjectButton(ZSlate::FSlateStandaloneApp& app,
                                                       const std::filesystem::path& project_file,
                                                       std::filesystem::path&       picked)
    {
        auto button = std::make_shared<ZSlate::SButton>();
        auto body   = std::make_shared<ZSlate::SVerticalBox>();

        body->AddSlot(MakeLabel(WideToUtf8(project_file.stem().wstring()), 15.0f, ZSlate::Colors::White));
        body->AddSlot(MakeLabel(WideToUtf8(project_file.parent_path().wstring()),
                                11.0f,
                                ZSlate::UIColor(0.52f, 0.55f, 0.62f, 1.0f)));

        button->SetContent(body);
        button->OnClicked = [&app, &picked, project_file]()
        {
            picked = project_file;
            app.RequestExit();
        };
        return button;
    }

    std::shared_ptr<ZSlate::SWidget> BuildRoot(ZSlate::FSlateStandaloneApp&        app,
                                               const std::vector<std::filesystem::path>& recent,
                                               std::filesystem::path&              picked)
    {
        using namespace ZSlate;

        auto root = std::make_shared<SVerticalBox>();
        const UIColor kMuted(0.60f, 0.63f, 0.70f, 1.0f);

        root->AddSlot(MakeLabel("ZEngine Launcher", 26.0f, Colors::White));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 4)));
        root->AddSlot(MakeLabel("选择一个项目开始", 14.0f, UIColor(0.60f, 0.72f, 0.92f, 1.0f)));
        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 18)));

        if (!recent.empty())
        {
            root->AddSlot(MakeLabel("最近项目", 15.0f, UIColor(0.78f, 0.80f, 0.86f, 1.0f)));
            for (const auto& project_file : recent)
            {
                root->AddSlot(MakeProjectButton(app, project_file, picked));
                root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 4)));
            }
        }
        else
        {
            root->AddSlot(MakeLabel("还没有项目，用下面的按钮打开或新建一个。", 14.0f, kMuted));
        }

        root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 16)));
        root->AddSlot(MakeButton("打开项目…",
                                 [&app, &picked]()
                                 {
                                     const std::filesystem::path chosen = BrowseForProject(app.GetHwnd());
                                     if (chosen.empty()) return;
                                     picked = chosen;
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
                                 [&app, &picked, name_box, dir_box, status]()
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

                                     picked = created;
                                     app.RequestExit();
                                 }));
        root->AddSlot(status);

        return root;
    }
}  // namespace

bool Run(std::filesystem::path& out_project_file)
{
    const std::filesystem::path list_path   = RecentListPath();
    std::vector<std::filesystem::path> recent = LoadRecent(list_path);

    std::filesystem::path picked;

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

    app.SetRootContent(BuildRoot(app, recent, picked));
    app.Run();
    app.Shutdown();

    if (picked.empty()) return false;

    PushRecent(recent, picked);
    SaveRecent(list_path, recent);

    out_project_file = picked;
    return true;
}

}  // namespace ZEditorLauncher

#endif  // _WIN32
