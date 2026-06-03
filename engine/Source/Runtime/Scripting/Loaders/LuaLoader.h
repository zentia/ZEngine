#pragma once

class ILuaLoader
{
    virtual std::string ReadFile(const char* filepath, const char* debugpath) = 0;
};

class LuaDefaultLoader : public ILuaLoader
{
public:
    virtual std::string ReadFile(const char* filepath, const char* debugpath);
};