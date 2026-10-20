#pragma once

// =============================================================================
// AssetBundleManifest -- the build's dependency table (PR-PAK1).
//
// Unity's `.manifest` counterpart, stored as a plain `.zasset` so it rides the
// existing serializer instead of a bespoke JSON/text parser (there is no
// general-purpose JSON writer in ZRuntime).
//
// The manifest is what makes a bundle loadable in isolation: mount bundle X,
// read `X.dependencies`, mount those first, and every PPtr the assets inside
// X carry will resolve. Without it a player build would have to mount
// everything or fail at the first cross-asset reference.
// =============================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <EASTL/string.h>

#include <cstdint>
#include <string>
#include <vector>
#include "AssetBundleManifest.generated.h"

// NOTE: the serializer supports std::vector, not eastl::vector (see
// SerializeTraitsBase.h -- it needs a GetTypeString() the EASTL containers do
// not provide). Strings stay eastl::string like everywhere else in res_type.

/// One bundle produced by a build.
struct AssetBundleInfo
{
    DECLARE_SERIALIZE(AssetBundleInfo)

    /// Bundle name == file stem, e.g. "levels_forest". The file is
    /// `<output_dir>/<name>.zbundle`.
    eastl::string name;
    /// Registry-relative asset paths packed into this bundle.
    std::vector<eastl::string> asset_paths;
    /// Names of bundles that must be mounted before this one (shared deps).
    std::vector<eastl::string> dependencies;
    /// Total stored bytes of the bundle file at build time (informational:
    /// lets a loader pre-size a read buffer and lets CI regression-test size).
    int64_t size_bytes {0};
};

template<typename TransferFunction>
void AssetBundleInfo::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(asset_paths, "asset_paths");
    transfer.Transfer(dependencies, "dependencies");
    transfer.Transfer(size_bytes, "size_bytes");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AssetBundleInfo, , Transfer, void)

ZCLASS()
class AssetBundleManifest : public Object
{
    ZGENERATED_BODY(AssetBundleManifest)

public:
    /// Platform tag the build was produced for ("Standalone", "Android", ...).
    ZPROPERTY()
    eastl::string platform;
    /// Project content root the asset paths are relative to (as written at
    /// build time). The runtime uses it to normalise incoming absolute paths
    /// back to registry-relative keys.
    ZPROPERTY()
    eastl::string content_root;
    ZPROPERTY()
    std::vector<AssetBundleInfo> bundles;

    const AssetBundleInfo* FindBundle(const char* bundle_name) const;
};
