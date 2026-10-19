#pragma once

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/AssetPipeline/AnimationImporter/AnimationImporterSettings.h"

// PR-SK2: bakes FBX animation stacks into AnimationAsset + AnimSkelMap pairs.
//
// One `.zasset` pair is produced per animation stack in the source file:
//   <stem>.<stack>.anim.zasset        (AnimationAsset: clip + node name map)
//   <stem>.<stack>.animskelmap.zasset (AnimSkelMap:   channel -> bone index)
//
// Clip channels are DELTAS on the bind pose (see AGENTS.md 2.22 and
// Runtime/Function/Animation/Skinning.h), which is what
// `Skeleton::ApplyAnimation()` composes with `ComposeDelta()`.
class AnimationImporter : public AssetImporter
{
public:
    bool CanImport(const std::filesystem::path& file_path) const override;

    std::vector<std::string> GetSupportedExtensions() const override;

    bool Import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override;

    bool Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings) override;

    std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;
};
