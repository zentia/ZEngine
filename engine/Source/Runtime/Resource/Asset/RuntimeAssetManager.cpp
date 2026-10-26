#include "RuntimeAssetManager.h"

int RuntimeAssetManager::InsertPathNameInternal(const std::filesystem::path& path, bool create)
{
    // Fast path: this exact spelling was seen before.
    const std::string spelling = path.string();
    PathToStreamID::iterator found = m_PathToStreamID.find(spelling);
    if (found != m_PathToStreamID.end())
        return found->second;

    // One file, one index (see AssetManager::CanonicalPathKey). A new spelling
    // of a known file is remembered as an alias so the canonicalisation (which
    // may touch the file system) runs once per spelling.
    const std::string canonical = CanonicalPathKey(path);
    found = m_PathToStreamID.find(canonical);
    if (found != m_PathToStreamID.end())
    {
        const int32_t index = found->second;
        m_PathToStreamID.emplace(spelling, index);
        return index;
    }

    if (create)
    {
        const int32_t index = static_cast<int32_t>(m_PathNames.size());
        m_PathToStreamID.emplace(canonical, index);
        m_PathToStreamID.emplace(spelling, index);
        m_PathNames.push_back(spelling);
        AddStream();
        return index;
    }
    return -1;
}

const std::string& RuntimeAssetManager::PathIDToPathNameInternal(int pathID) const
{
    return m_PathNames[pathID];
}