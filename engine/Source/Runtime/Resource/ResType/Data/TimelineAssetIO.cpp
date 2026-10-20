#include "Runtime/Resource/ResType/Data/TimelineAssetIO.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"
#include "core/Log/LogSystem.h"

#include <unordered_set>
#include <utility>

namespace TimelineAssetIO
{
    namespace
    {
        void AppendObject(Object* object, std::unordered_set<Object*>& seen, std::vector<Object*>& objects, std::vector<int64_t>& file_ids)
        {
            if (object == nullptr || !seen.insert(object).second)
                return;
            if (object->GetInstanceID() == 0)
                GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(object);
            objects.push_back(object);
            file_ids.push_back(static_cast<int64_t>(objects.size()));
        }
    }  // namespace

    void CollectObjectGraph(TimelineAsset& asset, std::vector<Object*>& objects, std::vector<int64_t>& file_ids)
    {
        objects.clear();
        file_ids.clear();
        std::unordered_set<Object*> seen;

        AppendObject(&asset, seen, objects, file_ids);
        for (const PPtr<TimelineTrack>& track_ptr : asset.m_Tracks)
        {
            TimelineTrack* track = track_ptr;
            if (track == nullptr)
                continue;
            AppendObject(track, seen, objects, file_ids);
            for (const PPtr<TimelineClip>& clip_ptr : track->m_Clips)
                AppendObject(static_cast<TimelineClip*>(clip_ptr), seen, objects, file_ids);
        }
    }

    bool Save(AssetManager& asset_manager, TimelineAsset& asset, const std::filesystem::path& path)
    {
        std::vector<Object*> objects;
        std::vector<int64_t> file_ids;
        CollectObjectGraph(asset, objects, file_ids);

        if (AssetManager::IsYamlAuthoringAssetPath(path))
            return asset_manager.WriteObjectsToYaml(path, objects.data(), file_ids.data(), objects.size());
        return asset_manager.WriteObjectsToDiskThreadSafe(path, objects.data(), file_ids.data(), objects.size());
    }

    TimelineAsset* Load(AssetManager& asset_manager, const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            LOG_ERROR(ZAsset, "TimelineAssetIO::Load: file not found {}", path.generic_string());
            return nullptr;
        }

        const bool is_yaml = AssetManager::IsYamlAuthoringAssetPath(path);
        if (!is_yaml)
        {
            // Binary graphs register their objects under path-derived
            // InstanceIDs; a second hydrate would re-Produce the same IDs.
            const int32_t cached_id = asset_manager.GetInstanceIDFromPathAndFileID(path, 1);
            if (Object* cached = cached_id != 0 ? GET_SYSTEM(ObjectManager)->IDToPointer(cached_id) : nullptr)
            {
                if (cached->Is(TypeOf<TimelineAsset>()))
                    return static_cast<TimelineAsset*>(cached);
            }
        }

        std::vector<std::pair<int64_t, Object*>> graph;
        const bool read = is_yaml ? asset_manager.ReadObjectsFromYaml(path, graph) : asset_manager.ReadObjectsFromBinary(path, graph);
        if (!read)
        {
            LOG_ERROR(ZAsset, "TimelineAssetIO::Load: failed to read {}", path.generic_string());
            return nullptr;
        }

        TimelineAsset* asset = nullptr;
        for (const auto& entry : graph)
        {
            if (entry.second != nullptr && entry.second->Is(TypeOf<TimelineAsset>()) && (asset == nullptr || entry.first == 1))
                asset = static_cast<TimelineAsset*>(entry.second);
        }
        if (asset == nullptr)
            LOG_ERROR(ZAsset, "TimelineAssetIO::Load: {} contains no TimelineAsset", path.generic_string());
        return asset;
    }
}  // namespace TimelineAssetIO
