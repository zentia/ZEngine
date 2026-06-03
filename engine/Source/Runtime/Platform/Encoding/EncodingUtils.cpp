#include "EncodingUtils.h"

#ifdef _WIN32
    #include <locale>
    #include <vector>
    #include <windows.h>
#endif

namespace Encoding
{
    void InitializeUtf8Locale()
    {
#ifdef _WIN32
        // Try to set locale to UTF-8
        // Note: This may not work on all Windows versions/compilers
        // MSVC 2019+ supports UTF-8 locale
        try
        {
            std::locale::global(std::locale(".UTF-8"));
        }
        catch (...)
        {
            // If UTF-8 locale is not supported, try English locale
            try
            {
                std::locale::global(std::locale("en_US.UTF-8"));
            }
            catch (...)
            {
                // If that also fails, try C locale (English)
                try
                {
                    std::locale::global(std::locale("C"));
                }
                catch (...)
                {
                    // If all fails, just use default locale
                }
            }
        }

        // Also set console code page to UTF-8 if possible
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    }

    std::string gbkToUtf8(const std::string& gbk_str)
    {
#ifdef _WIN32
        // First, convert GBK to wide string (UTF-16)
        int wide_size = MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, nullptr, 0);
        if (wide_size <= 0)
        {
            // If conversion fails, return original string
            return gbk_str;
        }

        std::vector<wchar_t> wide_str(wide_size);
        if (MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, wide_str.data(), wide_size) <= 0)
        {
            return gbk_str;
        }

        // Then, convert wide string (UTF-16) to UTF-8
        int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide_str.data(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8_size <= 0)
        {
            return gbk_str;
        }

        std::vector<char> utf8_str(utf8_size);
        if (WideCharToMultiByte(CP_UTF8, 0, wide_str.data(), -1, utf8_str.data(), utf8_size, nullptr, nullptr) <= 0)
        {
            return gbk_str;
        }

        return std::string(utf8_str.data());
#else
        // On non-Windows platforms, assume string is already UTF-8
        return gbk_str;
#endif
    }

    std::string GetFilesystemErrorMessage(const std::filesystem::filesystem_error& e)
    {
#ifdef _WIN32
        // On Windows, std::filesystem error messages are in GBK encoding
        // Convert to UTF-8
        return gbkToUtf8(e.what());
#else
        // On other platforms, assume UTF-8
        return e.what();
#endif
    }

    std::string GetExceptionMessage(const std::exception& e)
    {
#ifdef _WIN32
        // Try to convert GBK to UTF-8
        // This is a best-effort conversion
        return gbkToUtf8(e.what());
#else
        return e.what();
#endif
    }
}  // namespace Encoding