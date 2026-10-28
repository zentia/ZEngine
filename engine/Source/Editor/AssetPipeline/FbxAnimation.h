#pragma once

// =============================================================================
// PR-SK2: FBX animation baking, shared by the mesh and animation importers.
//
// Why shared: `AssetImportManager::FindImporter()` dispatches on extension and
// `.fbx` is already claimed by MeshImporter, so a standalone animation
// importer would never be reached. Dropping an FBX therefore produces mesh +
// skeleton + clips in ONE pass (Unity's ModelImporter behaviour), and both
// importers call the same `BakeAllStacks()`.
//
// Header-only under `ZENGINE_HAS_UFBX` for the same reason as FbxSkeleton.h.
//
// Output per animation stack:
//   <base>.<stack>.anim.zasset        -> AnimationAsset (clip + node name map)
//   <base>.<stack>.animskelmap.zasset -> AnimSkelMap (channel -> bone index)
//
// Clip channels are DELTAS on the bind pose; see
// Runtime/Function/Animation/Skinning.h and AGENTS.md 2.22.
// =============================================================================

#include "Editor/AssetPipeline/FbxSkeleton.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#if defined(ZENGINE_HAS_UFBX)

    #include <ufbx.h>

namespace FbxAnimation
{
    /// Produces an Object of type T, fills it through `in_setup` and writes it.
    template<typename T, typename SetupFn>
    bool WriteAsset(const std::filesystem::path& in_path, SetupFn in_setup)
    {
        auto* object_manager = GET_SYSTEM(ObjectManager);
        auto* asset_manager = GET_SYSTEM(AssetManager);
        if (object_manager == nullptr || asset_manager == nullptr)
        {
            return false;
        }

        Object* produced = object_manager->Produce(TypeOf<T>(), 0);
        if (produced == nullptr)
        {
            return false;
        }

        auto* typed = static_cast<T*>(produced);
        in_setup(*typed);

        std::error_code ec;
        if (!in_path.parent_path().empty())
        {
            std::filesystem::create_directories(in_path.parent_path(), ec);
        }

        const bool write_ok = asset_manager->WriteObjectToDiskThreadSafe(in_path, *typed);
        object_manager->Destroy(produced);
        return write_ok;
    }

    /// Safety net: 10 minutes at 60 Hz would be 36k keys per channel.
    constexpr int kAbsoluteMaxFrames = 10000;

    /// Node of every bone, in the same order `FbxSkeleton::Build()` numbers
    /// them, so channel N always maps to bone N.
    inline std::vector<const ufbx_node*> CollectBoneNodes(const ufbx_scene* scene)
    {
        std::vector<const ufbx_node*> nodes;
        for (size_t bone_i = 0; bone_i < scene->bones.count; ++bone_i)
        {
            const ufbx_bone* bone = scene->bones.data[bone_i];
            if (bone == nullptr || bone->instances.count == 0)
            {
                continue;
            }
            const ufbx_node* node = bone->instances.data[0];
            if (node == nullptr)
            {
                continue;
            }
            nodes.push_back(node);
        }
        return nodes;
    }

    inline Vector3 SafeDivide(const Vector3& in_lhs, const Vector3& in_rhs)
    {
        const auto divide = [](float in_value, float in_divisor) {
            return (std::fabs(in_divisor) > 1.0e-6f) ? (in_value / in_divisor) : 1.0f;
        };
        return Vector3(divide(in_lhs.x, in_rhs.x), divide(in_lhs.y, in_rhs.y), divide(in_lhs.z, in_rhs.z));
    }

    struct BakedClip
    {
        AnimationClip clip;
        /// Bone index per channel (becomes AnimSkelMap::convert).
        std::vector<int> bone_for_channel;
    };

    inline bool BakeStack(const ufbx_scene* scene,
                          const ufbx_anim_stack* stack,
                          const std::vector<const ufbx_node*>& in_bone_nodes,
                          float in_sample_rate,
                          int in_max_frames,
                          BakedClip& out_clip,
                          std::string& out_error)
    {
        ufbx_error err = {};
        ufbx_bake_opts bake_opts = {};
        ufbx_baked_anim* baked = ufbx_bake_anim(scene, stack->anim, &bake_opts, &err);
        if (baked == nullptr)
        {
            char buf[512] = {};
            ufbx_format_error(buf, sizeof(buf), &err);
            out_error = buf[0] != '\0' ? buf : "ufbx_bake_anim failed";
            return false;
        }

        const double duration =
            (baked->playback_duration > 0.0) ? baked->playback_duration : (stack->time_end - stack->time_begin);
        double fps = (in_sample_rate > 0.0f) ? static_cast<double>(in_sample_rate) : scene->settings.frames_per_second;
        if (!(fps > 0.0))
        {
            fps = 30.0;
        }

        const int cap = (in_max_frames > 0) ? std::min(in_max_frames, kAbsoluteMaxFrames) : kAbsoluteMaxFrames;
        const int frame_count = std::clamp(static_cast<int>(duration * fps) + 1, 2, cap);

        out_clip.clip.total_frame = frame_count;
        // P0.1: the clip's length in seconds, straight from the FBX. Correct
        // even when `frame_count` was clamped by `cap`, because the sample loop
        // below always spans the whole `duration` -- clamping lowers the sample
        // RATE, it does not shorten the clip.
        out_clip.clip.duration = static_cast<float>(duration);
        out_clip.clip.node_count = static_cast<int>(in_bone_nodes.size());
        out_clip.clip.node_channels.clear();
        out_clip.bone_for_channel.clear();

        for (size_t bone_index = 0; bone_index < in_bone_nodes.size(); ++bone_index)
        {
            const ufbx_node* node = in_bone_nodes[bone_index];

            const Vector3 bind_position = FbxSkeleton::ToVector3(node->local_transform.translation);
            const Quaternion bind_rotation = FbxSkeleton::ToQuaternion(node->local_transform.rotation);
            const Vector3 bind_scale = FbxSkeleton::ToVector3(node->local_transform.scale);

            ufbx_baked_node* baked_node = ufbx_find_baked_node(baked, const_cast<ufbx_node*>(node));

            AnimationChannel channel;
            channel.name = FbxSkeleton::ToName(node->name);
            channel.position_keys.reserve(static_cast<size_t>(frame_count));
            channel.rotation_keys.reserve(static_cast<size_t>(frame_count));
            channel.scaling_keys.reserve(static_cast<size_t>(frame_count));

            for (int frame = 0; frame < frame_count; ++frame)
            {
                const double time = baked->playback_time_begin +
                                    duration * (static_cast<double>(frame) / static_cast<double>(frame_count - 1));

                Vector3 animated_position = bind_position;
                Quaternion animated_rotation = bind_rotation;
                Vector3 animated_scale = bind_scale;
                if (baked_node != nullptr)
                {
                    animated_position = FbxSkeleton::ToVector3(ufbx_evaluate_baked_vec3(baked_node->translation_keys, time));
                    animated_rotation = FbxSkeleton::ToQuaternion(ufbx_evaluate_baked_quat(baked_node->rotation_keys, time));
                    animated_scale = FbxSkeleton::ToVector3(ufbx_evaluate_baked_vec3(baked_node->scale_keys, time));
                }

                // DELTAS on the bind pose -- composed at runtime by
                // AnimationSkinning::ComposeDelta().
                channel.position_keys.push_back(animated_position - bind_position);

                Quaternion rotation_delta = animated_rotation * bind_rotation.conjugate();
                rotation_delta.normalise();
                channel.rotation_keys.push_back(rotation_delta);

                channel.scaling_keys.push_back(SafeDivide(animated_scale, bind_scale));
            }

            out_clip.clip.node_channels.push_back(std::move(channel));
            out_clip.bone_for_channel.push_back(static_cast<int>(bone_index));
        }

        ufbx_free_baked_anim(baked);
        return true;
    }

    /// Bakes every stack of an already-loaded scene into clip + skel-map assets.
    /// `in_base_output_path` must already have its extension stripped. Returns
    /// the number of clips written.
    inline int BakeAllStacks(const ufbx_scene* scene,
                             const std::filesystem::path& in_base_output_path,
                             float in_sample_rate,
                             int in_max_frames)
    {
        const std::vector<const ufbx_node*> bone_nodes = CollectBoneNodes(scene);
        if (bone_nodes.empty() || scene->anim_stacks.count == 0)
        {
            return 0;
        }

        int written = 0;
        for (size_t stack_i = 0; stack_i < scene->anim_stacks.count; ++stack_i)
        {
            const ufbx_anim_stack* stack = scene->anim_stacks.data[stack_i];
            if (stack == nullptr || stack->anim == nullptr)
            {
                continue;
            }

            BakedClip baked;
            std::string bake_error;
            if (!BakeStack(scene, stack, bone_nodes, in_sample_rate, in_max_frames, baked, bake_error))
            {
                LOG_WARNING(ZEditor, "Animation bake: skipping stack {}: {}", stack_i, bake_error);
                continue;
            }

            std::string stack_name = "clip";
            if (stack->name.data != nullptr && stack->name.length > 0)
            {
                stack_name.clear();
                for (size_t c = 0; c < stack->name.length; ++c)
                {
                    const char ch = stack->name.data[c];
                    stack_name.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
                }
            }

            const std::string suffix = (scene->anim_stacks.count > 1) ? ("." + stack_name) : std::string();
            const std::string base_string = in_base_output_path.generic_string();
            const std::filesystem::path clip_path(base_string + suffix + ".anim.zasset");
            const std::filesystem::path skelmap_path(base_string + suffix + ".animskelmap.zasset");

            AnimationClip clip_copy = baked.clip;
            const std::vector<int> bone_for_channel = baked.bone_for_channel;
            const std::vector<AnimationChannel>& channels = clip_copy.node_channels;

            const float clip_duration = clip_copy.duration;
            const bool clip_ok = WriteAsset<AnimationAsset>(clip_path, [&channels, clip_duration](AnimationAsset& out_asset) {
                out_asset.clip_data.node_count = static_cast<int>(channels.size());
                out_asset.clip_data.node_channels = channels;
                out_asset.clip_data.total_frame = channels.empty() ? 0 : static_cast<int>(channels[0].position_keys.size());
                // P0.1: carried through from BakeStack. This lambda rebuilds the
                // clip field by field, so a field not copied here is silently
                // dropped -- which is exactly what happened to `duration` until
                // this line existed.
                out_asset.clip_data.duration = clip_duration;
                out_asset.node_map.convert.resize(channels.size());
                for (size_t channel_i = 0; channel_i < channels.size(); ++channel_i)
                {
                    out_asset.node_map.convert[channel_i] = channels[channel_i].name;
                }
                // skeleton_file_path stays empty: it is filled when the clip is
                // wired to a mesh's skeleton in the editor.
            });

            const bool map_ok = WriteAsset<AnimSkelMap>(skelmap_path, [&bone_for_channel](AnimSkelMap& out_map) {
                out_map.convert = bone_for_channel;
            });

            if (clip_ok && map_ok)
            {
                ++written;
                LOG_INFO(ZEditor,
                         "Animation bake: {} -> {} ({} channels x {} frames)",
                         stack_name,
                         clip_path.generic_string(),
                         channels.size(),
                         clip_copy.total_frame);
            }
            else
            {
                LOG_ERROR(ZEditor, "Animation bake: failed to write {} / {}", clip_path.string(), skelmap_path.string());
            }
        }
        return written;
    }
}  // namespace FbxAnimation

#endif
