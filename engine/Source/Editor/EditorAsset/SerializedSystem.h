#pragma once
#include <cstdint>
#include <filesystem>
#if defined(_WIN32)
    #include <guiddef.h>
#else
struct GUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
#endif
#include <string_view>

class Object;

class SerializedSystem
{
public:
    SerializedSystem(Object* ptr, std::filesystem::path& p, std::string_view& c, GUID& g, bool a)
        : pointer(ptr), path(p), className(c), guid(g), allowSerializeAsText(a)
    {
    }
    Object* pointer;
    std::filesystem::path path;
    std::string_view className;
    GUID guid;
    bool allowSerializeAsText;
};
