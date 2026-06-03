#include "Runtime/Resource/Asset/AssetManager.h"

class RuntimeAssetManager : public AssetManager
{
protected:
    virtual int InsertPathNameInternal(const std::filesystem::path& path, bool create);
    virtual const std::string& PathIDToPathNameInternal(int pathID) const;

private:
    using PathToStreamID = std::unordered_map<std::string, int32_t>;
    PathToStreamID m_PathToStreamID;
    std::vector<std::string> m_PathNames;
};