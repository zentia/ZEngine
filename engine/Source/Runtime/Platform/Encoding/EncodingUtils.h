#pragma once

#include <filesystem>
#include <string>

namespace Encoding
{
    /**
     * @brief Convert GBK string to UTF-8 string (Windows specific)
     * @param gbk_str The GBK encoded string
     * @return UTF-8 encoded string
     */
    std::string gbkToUtf8(const std::string& gbk_str);

    /**
     * @brief Get UTF-8 encoded error message from std::filesystem exception
     * On Windows, std::filesystem exceptions return GBK encoded messages,
     * this function converts them to UTF-8
     * @param e The filesystem exception
     * @return UTF-8 encoded error message
     */
    std::string GetFilesystemErrorMessage(const std::filesystem::filesystem_error& e);

    /**
     * @brief Get UTF-8 encoded error message from std::exception
     * Attempts to convert GBK to UTF-8 if on Windows
     * @param e The exception
     * @return UTF-8 encoded error message
     */
    std::string GetExceptionMessage(const std::exception& e);

    /**
     * @brief Initialize locale to UTF-8 (if supported)
     * Should be called at program startup
     */
    void InitializeUtf8Locale();
}  // namespace Encoding