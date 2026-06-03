#include "Editor/Platform/Interface/EditorUtility.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "Runtime/Function/Render/WindowSystem.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>
#include <filesystem>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace
{
    std::wstring utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        int wide_size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (wide_size <= 0)
        {
            return {};
        }

        std::vector<wchar_t> buffer(static_cast<size_t>(wide_size));
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer.data(), wide_size);
        return std::wstring(buffer.data());
    }

    std::string wideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

        int utf8_size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8_size <= 0)
        {
            return {};
        }

        std::vector<char> buffer(static_cast<size_t>(utf8_size));
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), utf8_size, nullptr, nullptr);
        return std::string(buffer.data());
    }

    bool beginDialogCOM(bool& com_initialized)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        com_initialized = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE)
        {
            com_initialized = false;
            return true;
        }

        return SUCCEEDED(hr);
    }

    void setDialogFolder(IFileDialog* dialog, const std::string& default_directory)
    {
        if (dialog == nullptr || default_directory.empty())
        {
            return;
        }

        std::wstring directory = utf8ToWide(default_directory);
        if (directory.empty())
        {
            return;
        }

        IShellItem* folder_item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder_item));
        if (SUCCEEDED(hr) && folder_item != nullptr)
        {
            dialog->SetDefaultFolder(folder_item);
            dialog->SetFolder(folder_item);
            folder_item->Release();
        }
    }

    HWND getEditorWindowHandle()
    {
        GLFWwindow* glfw_window = GET_SYSTEM(WindowSystem)->GetWindow();
        if (glfw_window == nullptr)
        {
            return nullptr;
        }

        return glfwGetWin32Window(glfw_window);
    }

    void setCommonDialogOptions(IFileDialog* dialog, const std::string& title)
    {
        if (dialog == nullptr)
        {
            return;
        }

        if (!title.empty())
        {
            std::wstring wide_title = utf8ToWide(title);
            if (!wide_title.empty())
            {
                dialog->SetTitle(wide_title.c_str());
            }
        }

        COMDLG_FILTERSPEC filter_spec[] = {{L"ZEngine Layout Files", L"*.zlayout.json"},
                                           {L"JSON Files", L"*.json"},
                                           {L"All Files", L"*.*"}};
        dialog->SetFileTypes(ARRAYSIZE(filter_spec), filter_spec);
        dialog->SetFileTypeIndex(1);
    }
}  // namespace

void EditorUtility::RevealInFinder(eastl::string path)
{
    if (path.empty())
    {
        return;
    }

    std::filesystem::path fs_path(path.c_str());
    fs_path = fs_path.lexically_normal();
    if (fs_path.is_relative())
    {
        fs_path = std::filesystem::absolute(fs_path).lexically_normal();
    }

    std::error_code error_code;
    if (std::filesystem::exists(fs_path, error_code))
    {
        fs_path = std::filesystem::weakly_canonical(fs_path, error_code).lexically_normal();
    }
    else if (fs_path.has_parent_path() && std::filesystem::exists(fs_path.parent_path(), error_code))
    {
        fs_path = std::filesystem::weakly_canonical(fs_path.parent_path(), error_code).lexically_normal();
    }

    if (std::filesystem::is_directory(fs_path, error_code))
    {
        ShellExecuteW(nullptr, L"open", fs_path.wstring().c_str(), nullptr, nullptr, SW_NORMAL);
        return;
    }

    const std::wstring command = L"/select,\"" + fs_path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_NORMAL);
}

void EditorUtility::OpenInExternalEditor(const eastl::string path)
{
    if (path.empty())
        return;

    std::filesystem::path fs_path(path.c_str());
    fs_path = fs_path.lexically_normal();
    if (fs_path.is_relative())
        fs_path = std::filesystem::absolute(fs_path).lexically_normal();

    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec))
        return;
    fs_path = std::filesystem::weakly_canonical(fs_path, ec).lexically_normal();

    const std::wstring file_w = fs_path.wstring();
    // Quote the path so paths-with-spaces survive cmd parsing.
    const std::wstring args_w = L"\"" + file_w + L"\"";

    // 1. Honour user-specified override - lets people use Sublime, Rider, ...
    {
        wchar_t override_buf[1024] = {};
        DWORD n = GetEnvironmentVariableW(L"ZENGINE_EXTERNAL_EDITOR", override_buf, _countof(override_buf));
        if (n > 0 && n < _countof(override_buf))
        {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", override_buf, args_w.c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(r) > 32)
                return;
        }
    }

    // 2. VSCode. ShellExecute uses PATHEXT (which includes .CMD) when the
    // lpFile has no extension, so `code` resolves to `code.cmd` if VSCode
    // is on PATH. SE_ERR_FNF (2) and friends fall through to step 3.
    {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", L"code", args_w.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32)
            return;
    }

    // 3. OS file association (Notepad++, Notepad, whatever the user set).
    ShellExecuteW(nullptr, L"open", file_w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool EditorUtility::OpenFileDialog(const std::string& title,
                                   const std::string& default_directory,
                                   const std::string& default_file_name,
                                   std::string& out_path)
{
    out_path.clear();
    bool com_initialized = false;
    if (!beginDialogCOM(com_initialized))
    {
        return false;
    }

    IFileOpenDialog* file_dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&file_dialog));
    if (FAILED(hr) || file_dialog == nullptr)
    {
        if (com_initialized)
        {
            CoUninitialize();
        }
        return false;
    }

    setCommonDialogOptions(file_dialog, title);
    setDialogFolder(file_dialog, default_directory);

    if (!default_file_name.empty())
    {
        std::wstring wide_name = utf8ToWide(default_file_name);
        if (!wide_name.empty())
        {
            file_dialog->SetFileName(wide_name.c_str());
        }
    }

    DWORD options = 0;
    if (SUCCEEDED(file_dialog->GetOptions(&options)))
    {
        file_dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }

    hr = file_dialog->Show(getEditorWindowHandle());
    if (SUCCEEDED(hr))
    {
        IShellItem* result_item = nullptr;
        hr = file_dialog->GetResult(&result_item);
        if (SUCCEEDED(hr) && result_item != nullptr)
        {
            PWSTR file_path = nullptr;
            hr = result_item->GetDisplayName(SIGDN_FILESYSPATH, &file_path);
            if (SUCCEEDED(hr) && file_path != nullptr)
            {
                out_path = wideToUtf8(file_path);
                CoTaskMemFree(file_path);
            }
            result_item->Release();
        }
    }

    file_dialog->Release();
    if (com_initialized)
    {
        CoUninitialize();
    }

    return !out_path.empty();
}

bool EditorUtility::SaveFileDialog(const std::string& title,
                                   const std::string& default_directory,
                                   const std::string& default_file_name,
                                   std::string& out_path)
{
    out_path.clear();
    bool com_initialized = false;
    if (!beginDialogCOM(com_initialized))
    {
        return false;
    }

    IFileSaveDialog* file_dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&file_dialog));
    if (FAILED(hr) || file_dialog == nullptr)
    {
        if (com_initialized)
        {
            CoUninitialize();
        }
        return false;
    }

    setCommonDialogOptions(file_dialog, title);
    setDialogFolder(file_dialog, default_directory);
    file_dialog->SetDefaultExtension(L"json");

    if (!default_file_name.empty())
    {
        std::wstring wide_name = utf8ToWide(default_file_name);
        if (!wide_name.empty())
        {
            file_dialog->SetFileName(wide_name.c_str());
        }
    }

    DWORD options = 0;
    if (SUCCEEDED(file_dialog->GetOptions(&options)))
    {
        file_dialog->SetOptions(options | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }

    hr = file_dialog->Show(getEditorWindowHandle());
    if (SUCCEEDED(hr))
    {
        IShellItem* result_item = nullptr;
        hr = file_dialog->GetResult(&result_item);
        if (SUCCEEDED(hr) && result_item != nullptr)
        {
            PWSTR file_path = nullptr;
            hr = result_item->GetDisplayName(SIGDN_FILESYSPATH, &file_path);
            if (SUCCEEDED(hr) && file_path != nullptr)
            {
                out_path = wideToUtf8(file_path);
                CoTaskMemFree(file_path);
            }
            result_item->Release();
        }
    }

    file_dialog->Release();
    if (com_initialized)
    {
        CoUninitialize();
    }

    return !out_path.empty();
}
