#include "ResourceManager.h"

#include "Runtime/BaseClasses/Object.h"

Object* ResourceManager::Load(std::filesystem::path& path)
{
    auto&& found = GetPathRange(path);

    Object* obj = nullptr;
    for (auto&& i = found.first; i != found.second; i++)
    {
        if (!(i->second))
            continue;
    }
    return obj;
}

ResourceManager::range ResourceManager::GetPathRange(std::filesystem::path& path)
{
    return m_Container.equal_range(path.generic_string().c_str());
}