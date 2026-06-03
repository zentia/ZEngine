#include "RuntimeAssetManager.h"

int RuntimeAssetManager::InsertPathNameInternal(const std::filesystem::path& path, bool create)
{
    auto&& pathname = path.string();
    PathToStreamID::iterator found = m_PathToStreamID.find(pathname);
    if (found != m_PathToStreamID.end())
        return found->second;

    if (create)
    {
        m_PathToStreamID.insert(std::make_pair(pathname, m_PathNames.size()));
        m_PathNames.push_back(pathname);
        AddStream();
        return m_PathNames.size() - 1;
    }
    return -1;
}

const std::string& RuntimeAssetManager::PathIDToPathNameInternal(int pathID) const
{
    return m_PathNames[pathID];
}