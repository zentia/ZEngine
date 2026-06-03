#include "LuaLoader.h"

#include "Runtime/File/FileSystem.h"

#include <fstream>
#include <iostream>
#include <string>

std::string LuaDefaultLoader::ReadFile(const char* filepath, const char* debugpath)
{
    std::string content;
    std::filesystem::path p(filepath);
    FileSystem::ReadStringFromFile(&content, p);
    return content;
}