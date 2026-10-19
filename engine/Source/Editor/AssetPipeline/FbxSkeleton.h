#pragma once

// =============================================================================
// PR-SK2: shared FBX -> skeleton extraction.
//
// The mesh importer needs the bones to resolve per-vertex skin weights, and the
// animation importer needs the exact same bone numbering to build AnimSkelMap
// (`clip channel -> bone index`). If those two ever disagree the character
// silently animates the wrong bones, so the table is built in one place.
//
// Header-only: ufbx is only available to the editor target
// (`ZENGINE_HAS_UFBX`, see Editor/CMakeLists.txt), so everything here vanishes
// when it is not defined rather than pulling <ufbx.h> into unrelated TUs.
//
// Conventions (must match Runtime/Function/Animation/Skinning.h):
//   * bones are numbered 0-based in `bones[].index` == position in the vector;
//   * `binding_pose` is the bone node's LOCAL transform at bind time -- the
//     runtime composes the global bind matrices and inverts them itself;
//   * `tpose_matrix` is left at identity: nothing consumes it.
// =============================================================================

#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <map>
#include <vector>

#if defined(ZENGINE_HAS_UFBX)

    #include <ufbx.h>

namespace FbxSkeleton
{
    inline eastl::string ToName(const ufbx_string& in_name)
    {
        if (in_name.data == nullptr || in_name.length == 0)
        {
            return eastl::string();
        }
        return eastl::string(in_name.data, in_name.length);
    }

    inline Vector3 ToVector3(const ufbx_vec3& in_value)
    {
        return Vector3(static_cast<float>(in_value.x), static_cast<float>(in_value.y), static_cast<float>(in_value.z));
    }

    inline Quaternion ToQuaternion(const ufbx_quat& in_value)
    {
        return Quaternion(static_cast<float>(in_value.w),
                          static_cast<float>(in_value.x),
                          static_cast<float>(in_value.y),
                          static_cast<float>(in_value.z));
    }

    /// Bone list of a scene plus a node -> bone lookup.
    struct BoneTable
    {
        std::vector<RawBone> bones;
        std::map<const ufbx_node*, int32_t> node_to_bone;

        bool HasBones() const { return !bones.empty(); }

        /// Bone index of `in_node`, walking up to the nearest bone ancestor.
        /// FBX skin clusters frequently point at a helper/null node parented
        /// under the real bone, which is why this walks the chain.
        int32_t FindBoneNode(const ufbx_node* in_node) const
        {
            for (const ufbx_node* node = in_node; node != nullptr; node = node->parent)
            {
                const auto found = node_to_bone.find(node);
                if (found != node_to_bone.end())
                {
                    return found->second;
                }
            }
            return -1;
        }
    };

    /// Collects `scene->bones` into a flat table and resolves parents to the
    /// nearest bone ancestor. Order does not have to be parent-first:
    /// `Skeleton::BuildSkeleton()` sorts topologically.
    inline BoneTable Build(const ufbx_scene* scene)
    {
        BoneTable table;
        if (scene == nullptr)
        {
            return table;
        }

        std::vector<const ufbx_node*> bone_nodes(scene->bones.count, nullptr);
        for (size_t bone_i = 0; bone_i < scene->bones.count; ++bone_i)
        {
            const ufbx_bone* bone = scene->bones.data[bone_i];
            if (bone == nullptr)
            {
                continue;
            }
            const ufbx_node* node = (bone->instances.count > 0) ? bone->instances.data[0] : nullptr;
            bone_nodes[bone_i] = node;
            if (node == nullptr)
            {
                continue;
            }

            RawBone raw_bone;
            raw_bone.name = ToName(node->name);
            raw_bone.index = static_cast<int>(table.bones.size());
            raw_bone.parent_index = -1;
            raw_bone.binding_pose = TransformTRS(ToVector3(node->local_transform.translation),
                                                 ToQuaternion(node->local_transform.rotation),
                                                 ToVector3(node->local_transform.scale));

            table.node_to_bone[node] = raw_bone.index;
            table.bones.push_back(std::move(raw_bone));
        }

        for (size_t bone_i = 0; bone_i < bone_nodes.size(); ++bone_i)
        {
            const ufbx_node* node = bone_nodes[bone_i];
            if (node == nullptr)
            {
                continue;
            }
            const auto self = table.node_to_bone.find(node);
            if (self == table.node_to_bone.end())
            {
                continue;
            }
            for (const ufbx_node* parent = node->parent; parent != nullptr; parent = parent->parent)
            {
                const auto found = table.node_to_bone.find(parent);
                if (found != table.node_to_bone.end())
                {
                    table.bones[static_cast<size_t>(self->second)].parent_index = found->second;
                    break;
                }
            }
        }
        return table;
    }
}  // namespace FbxSkeleton

#endif
