#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

class AssetManager;
class Object;
class TimelineAsset;

// =====================================================================
// TimelineAssetIO -- persistence for TimelineAsset object graphs.
// ---------------------------------------------------------------------
// A timeline is a multi-object file (Unity .playable layout):
//
//   fileID 1        TimelineAsset
//   fileID 2..      each TimelineTrack, immediately followed by its clips
//
// Tracks and clips are sub-objects referenced through PPtr
// (TimelineAsset::m_Tracks, TimelineTrack::m_Clips), so the file must be
// written and read as ONE graph. The single-object paths
// (AssetManager::loadAsset<T> / ReadObject(path, type)) hydrate only
// fileID 1 and would leave every track / clip reference dangling.
//
// Format follows the extension: ".timeline" is a YAML authoring asset
// (diff-friendly, like .prefab / .scene); anything else (".zasset") is the
// binary SerializedFile.
// =====================================================================
namespace TimelineAssetIO
{
    /// Flatten `asset` into graph order (see above). Objects without an
    /// InstanceID get a heap ID first; a clip / track referenced twice is
    /// written once. Null PPtrs are skipped.
    void CollectObjectGraph(TimelineAsset& asset, std::vector<Object*>& objects, std::vector<int64_t>& file_ids);

    /// Write the whole graph to `path` (YAML for .timeline, binary otherwise).
    bool Save(AssetManager& asset_manager, TimelineAsset& asset, const std::filesystem::path& path);

    /// Read the whole graph from `path` and return its TimelineAsset (fileID 1),
    /// with every m_Tracks / m_Clips PPtr resolving to the freshly produced
    /// sub-objects. Returns nullptr when the file is missing or holds no
    /// TimelineAsset. Binary files reuse an already-loaded instance (their
    /// InstanceIDs are path-derived); YAML loads always produce a new graph,
    /// same as prefabs. The caller owns the produced objects.
    TimelineAsset* Load(AssetManager& asset_manager, const std::filesystem::path& path);
}  // namespace TimelineAssetIO
