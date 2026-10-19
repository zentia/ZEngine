#include "AssetBundleManifest.h"

IMPLEMENT_REGISTER_CLASS(AssetBundleManifest);
IMPLEMENT_OBJECT_SERAILIZE(AssetBundleManifest);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AssetBundleManifest)

template<typename TransferFunction>
void AssetBundleManifest::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(platform, "platform");
    transfer.Transfer(content_root, "content_root");
    transfer.Transfer(bundles, "bundles");
}

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
