#pragma once

#include <string>

namespace zengine::envcheck
{
// Escapes a UTF-8 string for use inside a JSON double-quoted literal. The
// surrounding quotes are NOT emitted, so callers stay in control of whether
// they are building a compact one-line document or an indented one.
//
// Shared by every JSON writer in this library (ReportWriter.cpp,
// GpuSelectionProbe.cpp): the collector's stage JSON and the compatibility
// report's JSON are read by the same people, so their escaping rules must not
// be able to drift apart.
inline std::string EscapeJson(const std::string& value)
{
    static const char* kHexDigits = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result += kHexDigits[character >> 4];
                result += kHexDigits[character & 0xf];
            }
            else
            {
                result += static_cast<char>(character);
            }
            break;
        }
    }
    return result;
}
} // namespace zengine::envcheck
