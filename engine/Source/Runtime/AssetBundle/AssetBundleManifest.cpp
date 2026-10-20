#include "AssetBundleManifest.h"


const AssetBundleInfo* AssetBundleManifest::FindBundle(const char* bundle_name) const
{
    if (bundle_name == nullptr)
    {
        return nullptr;
    }
    for (const AssetBundleInfo& info : bundles)
    {
        if (info.name == bundle_name)
        {
            return &info;
        }
    }
    return nullptr;
}
