// ============================================================================
// SlateWin32Host — see include/ZSlate/Platform/Win32/SlateWin32Host.h
// ============================================================================

#include "ZSlate/Platform/Win32/SlateWin32Host.h"

#if defined(_WIN32)

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Core/SlateClipboard.h"
#include "ZSlate/Core/SlateLog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <shellapi.h>  // CommandLineToArgvW

namespace ZSlate
{
namespace Win32
{

// ---- Fonts ---------------------------------------------------------------

bool LoadSystemFonts(ZSlateFontAtlas& atlas, ESystemFont preference)
{
    wchar_t windows_dir[MAX_PATH] {};
    if (GetWindowsDirectoryW(windows_dir, MAX_PATH) == 0)
        return false;

    const std::filesystem::path fonts = std::filesystem::path(windows_dir) / L"Fonts";

    // Primary candidates, best first; the loser stays as a fallback so a
    // machine missing the preferred face still renders rather than blocking.
    const wchar_t* sans[] = {L"segoeui.ttf", L"tahoma.ttf", L"arial.ttf"};
    const wchar_t* mono[] = {L"consola.ttf", L"cour.ttf"};

    const wchar_t* const* preferred = (preference == ESystemFont::Mono) ? mono : sans;
    const size_t   preferred_count  = (preference == ESystemFont::Mono)
                                          ? sizeof(mono) / sizeof(mono[0])
                                          : sizeof(sans) / sizeof(sans[0]);
    const wchar_t* const* other    = (preference == ESystemFont::Mono) ? sans : mono;
    const size_t   other_count     = (preference == ESystemFont::Mono)
                                          ? sizeof(sans) / sizeof(sans[0])
                                          : sizeof(mono) / sizeof(mono[0]);

    bool loaded = false;
    for (size_t i = 0; i < preferred_count && !loaded; ++i)
        loaded = atlas.LoadFromFile(Utf8FromWide((fonts / preferred[i]).c_str()));
    for (size_t i = 0; i < other_count && !loaded; ++i)
        loaded = atlas.LoadFromFile(Utf8FromWide((fonts / other[i]).c_str()));

    if (!loaded)
        return false;

    // CJK fallback: the primary faces above have no Han glyphs. Candidates are
    // ordered best-first; Simplified faces win, Traditional faces are the last
    // resort before the directory scan (a Traditional face still renders
    // Simplified text, just with a less familiar look).
    const wchar_t* cjk[] = {
        L"msyh.ttc", L"msyhlthc.ttc", L"simsun.ttc", L"simhei.ttf",
        L"Deng.ttf", L"msjh.ttc", L"simkai.ttf", L"simfang.ttf",
        L"mingliu.ttc", L"kaiu.ttf",
    };
    bool cjkLoaded = false;
    for (const wchar_t* f : cjk)
    {
        if (atlas.AddFallbackFont(Utf8FromWide((fonts / f).c_str())))
        {
            cjkLoaded = true;
            break;
        }
    }

    if (!cjkLoaded)
    {
        // Stripped-down Windows installs (Ghost/精简版/Server Core) routinely
        // lack every face above while still shipping *some* CJK-capable font
        // under a different name. Scan the directory: CJK faces are several MB
        // while Latin faces are ~1MB, so size is a cheap first filter; the
        // stb probe then confirms the face actually covers U+4E2D ("中").
        // Bounded to a handful of candidate loads to keep startup snappy.
        struct Candidate { std::filesystem::path path; uintmax_t size; };
        std::vector<Candidate> candidates;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(fonts, std::filesystem::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            const std::filesystem::path& p = it->path();
            std::error_code statEc;
            const bool regular = it->is_regular_file(statEc);
            const uintmax_t bytes = it->file_size(statEc);
            if (statEc || !regular) continue;
            const std::wstring ext = p.extension().wstring();
            if (_wcsicmp(ext.c_str(), L".ttf") != 0 &&
                _wcsicmp(ext.c_str(), L".ttc") != 0 &&
                _wcsicmp(ext.c_str(), L".otf") != 0)
                continue;
            if (bytes < 2u * 1024u * 1024u) continue;  // CJK faces are never this small
            candidates.push_back({p, bytes});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.size > b.size; });

        constexpr int kMaxProbeLoads = 8;
        int probed = 0;
        for (const Candidate& c : candidates)
        {
            if (probed >= kMaxProbeLoads) break;
            ++probed;
            const std::string utf8 = Utf8FromWide(c.path.c_str());
            if (ZSlateFontAtlas::ProbeHasCodepoint(utf8, 0x4E2D) && atlas.AddFallbackFont(utf8))
            {
                cjkLoaded = true;
                break;
            }
        }
    }

    if (!cjkLoaded)
    {
        // Nothing on the machine covers Han glyphs; Chinese text will paint as
        // boxes. Keep the window alive (Latin still renders) but say so.
        Log(ELogLevel::Warning,
            "[ZSlate] no CJK-capable font found in '%s'; Chinese text will render as boxes",
            Utf8FromWide(fonts.c_str()).c_str());
    }
    return true;
}

// ---- Keys ----------------------------------------------------------------

EKey KeyFromVirtualKey(unsigned vk)
{
    switch (vk)
    {
    case VK_BACK:   return EKey::Backspace;
    case VK_DELETE: return EKey::Delete;
    case VK_RETURN: return EKey::Enter;
    case VK_ESCAPE: return EKey::Escape;
    case VK_TAB:    return EKey::Tab;
    case VK_LEFT:   return EKey::Left;
    case VK_RIGHT:  return EKey::Right;
    case VK_UP:     return EKey::Up;
    case VK_DOWN:   return EKey::Down;
    case VK_HOME:   return EKey::Home;
    case VK_END:    return EKey::End;
    case VK_SPACE:  return EKey::Space;
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<EKey>(static_cast<int>(EKey::A) + static_cast<int>(vk - 'A'));
    return EKey::Unknown;
}

// ---- Clipboard -----------------------------------------------------------

namespace
{
// The ZSlate getter returns a bare `const char*`, so the bytes have to outlive
// the call. A process-wide buffer is what every host ends up doing; one copy
// here beats one per tool.
std::string& ClipboardBuffer()
{
    static std::string buffer;
    return buffer;
}

const char* GetClipboardText(void*)
{
    std::string& out = ClipboardBuffer();
    out.clear();
    if (!OpenClipboard(nullptr))
        return out.c_str();

    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT))
    {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data)))
        {
            out = Utf8FromWide(wide);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out.c_str();
}

void SetClipboardText(const char* text, void*)
{
    if (text == nullptr || !OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    const std::wstring wide = WideFromUtf8(text);
    if (wide.empty())
    {
        CloseClipboard();
        return;
    }

    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        if (auto* destination = static_cast<wchar_t*>(GlobalLock(memory)))
        {
            std::memcpy(destination, wide.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        }
        else
        {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}
}  // namespace

void InstallClipboardBridge()
{
    SlateApplication::Get().SetClipboardCallbacks(&GetClipboardText, &SetClipboardText, nullptr);
}

// ---- Strings -------------------------------------------------------------

std::string Utf8FromWide(const wchar_t* wide)
{
    if (wide == nullptr)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring WideFromUtf8(const char* utf8)
{
    if (utf8 == nullptr)
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], size);
    return out;
}

std::vector<std::string> CommandLineArgsUtf8()
{
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
        return args;
    for (int i = 0; i < argc; ++i)
        args.push_back(Utf8FromWide(argv[i]));
    LocalFree(argv);
    return args;
}

// The console helpers that used to be here now live in ZAppShell
// (zengine::appshell::EnsureConsole), header-only so that both the /MT
// ZEnvironmentCollector and the /MD tree can use one implementation -- which a
// compiled-into-ZSlate version could never do.

}  // namespace Win32
}  // namespace ZSlate

#endif  // _WIN32
