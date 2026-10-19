#include "Runtime/Function/Animation/Skinning.h"

#include <algorithm>
#include <cmath>

namespace AnimationSkinning
{
    namespace
    {
        constexpr float kPoseEpsilon = 1.0e-6f;

        /// Slot of a bone in `in_bones`, or -1. Handles both the flat layout
        /// (index == position) and skeletons whose `index` field disagrees.
        int32_t FindSlot(const std::vector<RawBone>& in_bones, int32_t in_bone_index)
        {
            if (in_bone_index >= 0 && static_cast<size_t>(in_bone_index) < in_bones.size() &&
                in_bones[static_cast<size_t>(in_bone_index)].index == in_bone_index)
            {
                return in_bone_index;
            }
            for (size_t slot = 0; slot < in_bones.size(); ++slot)
            {
                if (in_bones[slot].index == in_bone_index)
                {
                    return static_cast<int32_t>(slot);
                }
            }
            return -1;
        }
    }  // namespace

    Matrix4x4 BonePose::GetMatrix() const
    {
        Matrix4x4 result;
        result.MakeTransform(m_Position, m_Scale, m_Rotation);
        return result;
    }

    int32_t ResolveParentIndex(const std::vector<RawBone>& in_bones, size_t in_bone_index)
    {
        if (in_bone_index >= in_bones.size())
        {
            return -1;
        }

        const RawBone& bone = in_bones[in_bone_index];
        if (bone.parent_index < 0)
        {
            return -1;
        }
        if (static_cast<size_t>(bone.parent_index) == in_bone_index)
        {
            return -1;  // Self-parent: unbreakable, treat as a root.
        }
        return FindSlot(in_bones, bone.parent_index);
    }

    bool EnsureTopologicalOrder(std::vector<RawBone>& in_out_bones)
    {
        const size_t bone_count = in_out_bones.size();
        if (bone_count == 0)
        {
            return true;
        }

        // Map every bone's declared index to its current slot so parent links can
        // be resolved even when the array is not in index order.
        std::vector<int32_t> slot_of_index(bone_count, -1);
        for (size_t slot = 0; slot < bone_count; ++slot)
        {
            const int32_t declared = in_out_bones[slot].index;
            if (declared >= 0 && static_cast<size_t>(declared) < bone_count)
            {
                slot_of_index[static_cast<size_t>(declared)] = static_cast<int32_t>(slot);
            }
        }

        std::vector<int32_t> parent_slot(bone_count, -1);
        std::vector<uint32_t> child_count(bone_count, 0);
        for (size_t slot = 0; slot < bone_count; ++slot)
        {
            const RawBone& bone = in_out_bones[slot];
            if (bone.parent_index < 0)
            {
                continue;
            }
            const int32_t parent = (static_cast<size_t>(bone.parent_index) < bone_count)
                                       ? slot_of_index[static_cast<size_t>(bone.parent_index)]
                                       : -1;
            if (parent < 0 || static_cast<size_t>(parent) == slot)
            {
                continue;  // Broken link or self-parent: bone becomes a root.
            }
            parent_slot[slot] = parent;
            ++child_count[static_cast<size_t>(parent)];
        }

        // Kahn: emit every root, then each parent's children.
        std::vector<size_t> order;
        order.reserve(bone_count);
        for (size_t slot = 0; slot < bone_count; ++slot)
        {
            if (parent_slot[slot] < 0)
            {
                order.push_back(slot);
            }
        }
        for (size_t cursor = 0; cursor < order.size(); ++cursor)
        {
            const size_t parent = order[cursor];
            for (size_t slot = 0; slot < bone_count; ++slot)
            {
                if (parent_slot[slot] >= 0 && static_cast<size_t>(parent_slot[slot]) == parent)
                {
                    order.push_back(slot);
                }
            }
        }

        if (order.size() != bone_count)
        {
            return false;  // Cycle: the hierarchy cannot be evaluated.
        }

        // Rebuild the array in the emitted order and renumber it flat, so that
        // afterwards slot == index == the position every other system assumes.
        std::vector<RawBone> sorted;
        sorted.reserve(bone_count);
        std::vector<int32_t> new_index_of_slot(bone_count, -1);
        for (size_t new_slot = 0; new_slot < order.size(); ++new_slot)
        {
            new_index_of_slot[order[new_slot]] = static_cast<int32_t>(new_slot);
        }
        for (size_t new_slot = 0; new_slot < order.size(); ++new_slot)
        {
            RawBone bone = in_out_bones[order[new_slot]];
            const int32_t old_parent = parent_slot[order[new_slot]];
            bone.index = static_cast<int32_t>(new_slot);
            bone.parent_index = (old_parent >= 0) ? new_index_of_slot[static_cast<size_t>(old_parent)] : -1;
            sorted.push_back(std::move(bone));
        }

        in_out_bones.swap(sorted);
        return true;
    }

    void ComputeGlobalBindMatrices(const std::vector<RawBone>& in_bones, std::vector<Matrix4x4>& out_global_bind)
    {
        out_global_bind.assign(in_bones.size(), Matrix4x4::IDENTITY);
        for (size_t slot = 0; slot < in_bones.size(); ++slot)
        {
            const Matrix4x4 local = in_bones[slot].binding_pose.getMatrix();
            const int32_t parent = ResolveParentIndex(in_bones, slot);
            out_global_bind[slot] = (parent >= 0 && static_cast<size_t>(parent) < slot)
                                        ? out_global_bind[static_cast<size_t>(parent)] * local
                                        : local;
        }
    }

    void ComputeInverseBindMatrices(const std::vector<RawBone>& in_bones, std::vector<Matrix4x4>& out_inverse_bind)
    {
        std::vector<Matrix4x4> global_bind;
        ComputeGlobalBindMatrices(in_bones, global_bind);

        out_inverse_bind.assign(global_bind.size(), Matrix4x4::IDENTITY);
        for (size_t slot = 0; slot < global_bind.size(); ++slot)
        {
            out_inverse_bind[slot] = global_bind[slot].inverse();
        }
    }

    void ComputeGlobalMatrices(const std::vector<RawBone>& in_bones,
                               const std::vector<BonePose>& in_local_poses,
                               std::vector<Matrix4x4>& out_global)
    {
        const size_t bone_count = in_bones.size();
        out_global.assign(bone_count, Matrix4x4::IDENTITY);
        if (in_local_poses.empty())
        {
            return;
        }

        for (size_t slot = 0; slot < bone_count; ++slot)
        {
            const BonePose local_pose = (slot < in_local_poses.size()) ? in_local_poses[slot] : BonePose::Identity();
            const Matrix4x4 local = local_pose.GetMatrix();
            const int32_t parent = ResolveParentIndex(in_bones, slot);
            out_global[slot] = (parent >= 0 && static_cast<size_t>(parent) < slot)
                                   ? out_global[static_cast<size_t>(parent)] * local
                                   : local;
        }
    }

    void BuildSkinningPalette(const std::vector<Matrix4x4>& in_global,
                              const std::vector<Matrix4x4>& in_inverse_bind,
                              std::vector<Matrix4x4>& out_palette)
    {
        const size_t count = std::min(in_global.size(), in_inverse_bind.size());
        out_palette.assign(count, Matrix4x4::IDENTITY);
        for (size_t slot = 0; slot < count; ++slot)
        {
            out_palette[slot] = in_global[slot] * in_inverse_bind[slot];
        }
    }

    void BuildGpuPaletteWithRootSlot(const std::vector<Matrix4x4>& in_palette, std::vector<Matrix4x4>& out_gpu_palette)
    {
        out_gpu_palette.assign(in_palette.size() + 1u, Matrix4x4::IDENTITY);
        out_gpu_palette[0] = Matrix4x4::IDENTITY;
        for (size_t slot = 0; slot < in_palette.size(); ++slot)
        {
            out_gpu_palette[slot + 1u] = in_palette[slot];
        }
    }

    float ApplyWrap(float in_normalized_time, WrapMode in_mode)
    {
        switch (in_mode)
        {
            case WrapMode::Clamp:
                return std::clamp(in_normalized_time, 0.0f, 1.0f);

            case WrapMode::Loop:
            {
                float wrapped = in_normalized_time - std::floor(in_normalized_time);
                if (wrapped < 0.0f)
                {
                    wrapped += 1.0f;
                }
                return wrapped;
            }

            case WrapMode::PingPong:
            default:
            {
                // Triangle wave: 0 -> 1 -> 0 with period 2.
                float wrapped = in_normalized_time - 2.0f * std::floor(in_normalized_time / 2.0f);
                return wrapped <= 1.0f ? wrapped : (2.0f - wrapped);
            }
        }
    }

    BonePose SampleChannel(const AnimationChannel& in_channel, float in_normalized_time, WrapMode in_mode)
    {
        BonePose pose;

        const size_t position_count = in_channel.position_keys.size();
        const size_t rotation_count = in_channel.rotation_keys.size();
        const size_t scaling_count = in_channel.scaling_keys.size();
        const size_t key_count = std::max(position_count, std::max(rotation_count, scaling_count));
        if (key_count == 0)
        {
            return pose;  // Identity: an unanimated bone keeps its bind pose.
        }

        const float time = ApplyWrap(in_normalized_time, in_mode);
        const float exact_key = time * static_cast<float>(key_count - 1);
        const int32_t key_low = static_cast<int32_t>(std::floor(exact_key));
        const float fraction = exact_key - static_cast<float>(key_low);

        auto index_for = [key_count](size_t in_array_size, int32_t in_key) {
            if (in_array_size == 0)
            {
                return size_t {0};
            }
            return static_cast<size_t>(std::min(static_cast<size_t>(std::max(in_key, 0)), in_array_size - 1));
        };

        if (position_count > 0)
        {
            pose.m_Position = Vector3::Lerp(in_channel.position_keys[index_for(position_count, key_low)],
                                            in_channel.position_keys[index_for(position_count, key_low + 1)],
                                            fraction);
        }
        if (scaling_count > 0)
        {
            pose.m_Scale = Vector3::Lerp(in_channel.scaling_keys[index_for(scaling_count, key_low)],
                                         in_channel.scaling_keys[index_for(scaling_count, key_low + 1)],
                                         fraction);
        }
        if (rotation_count > 0)
        {
            pose.m_Rotation = Quaternion::NLerp(
                fraction, in_channel.rotation_keys[index_for(rotation_count, key_low)],
                in_channel.rotation_keys[index_for(rotation_count, key_low + 1)], true);
        }

        return pose;
    }

    void SampleClip(const AnimationClip& in_clip, float in_normalized_time, WrapMode in_mode, std::vector<BonePose>& out_poses)
    {
        const size_t node_count = std::min(static_cast<size_t>(std::max(in_clip.node_count, 0)),
                                           in_clip.node_channels.size());
        out_poses.assign(node_count, BonePose::Identity());
        for (size_t node_index = 0; node_index < node_count; ++node_index)
        {
            out_poses[node_index] = SampleChannel(in_clip.node_channels[node_index], in_normalized_time, in_mode);
        }
    }

    void BlendPose(const BonePose& in_a, const BonePose& in_b, float in_weight, BonePose& out_pose)
    {
        const float clamped = std::clamp(in_weight, 0.0f, 1.0f);
        out_pose.m_Position = Vector3::Lerp(in_a.m_Position, in_b.m_Position, clamped);
        out_pose.m_Scale = Vector3::Lerp(in_a.m_Scale, in_b.m_Scale, clamped);
        out_pose.m_Rotation = Quaternion::NLerp(clamped, in_a.m_Rotation, in_b.m_Rotation, true);
    }

    BonePose ComposeDelta(const BonePose& in_bind, const BonePose& in_delta)
    {
        BonePose composed;
        composed.m_Position = in_bind.m_Position + in_delta.m_Position;
        composed.m_Rotation = in_delta.m_Rotation * in_bind.m_Rotation;
        composed.m_Scale = Vector3(in_bind.m_Scale.x * in_delta.m_Scale.x,
                                   in_bind.m_Scale.y * in_delta.m_Scale.y,
                                   in_bind.m_Scale.z * in_delta.m_Scale.z);
        composed.m_Rotation.normalise();
        return composed;
    }

    void AccumulateBlend(const BonePose& in_pose, float in_weight, BonePose& in_out_accumulator, float& in_out_total_weight)
    {
        if (in_weight <= 0.0f)
        {
            return;
        }

        // Keep every contribution in the same hemisphere, otherwise two nearly
        // opposite rotations cancel out instead of averaging.
        Quaternion rotation = in_pose.m_Rotation;
        if (rotation.w * in_out_accumulator.m_Rotation.w + rotation.x * in_out_accumulator.m_Rotation.x +
                rotation.y * in_out_accumulator.m_Rotation.y + rotation.z * in_out_accumulator.m_Rotation.z <
            0.0f)
        {
            rotation = rotation * -1.0f;
        }

        in_out_accumulator.m_Position = in_out_accumulator.m_Position + in_pose.m_Position * in_weight;
        in_out_accumulator.m_Scale = in_out_accumulator.m_Scale + in_pose.m_Scale * in_weight;
        in_out_accumulator.m_Rotation = in_out_accumulator.m_Rotation + rotation * in_weight;
        in_out_total_weight += in_weight;
    }

    void NormalizeBlendAccumulator(BonePose& in_out_accumulator, float in_total_weight)
    {
        if (in_total_weight <= kPoseEpsilon)
        {
            in_out_accumulator = BonePose::Identity();
            return;
        }

        const float inverse_weight = 1.0f / in_total_weight;
        in_out_accumulator.m_Position = in_out_accumulator.m_Position * inverse_weight;
        in_out_accumulator.m_Scale = in_out_accumulator.m_Scale * inverse_weight;
        Quaternion& rotation = in_out_accumulator.m_Rotation;
        const float length = rotation.length();
        if (length > kPoseEpsilon)
        {
            rotation = rotation * (1.0f / length);
        }
        else
        {
            rotation = Quaternion::IDENTITY;
        }
    }

    bool PackVertexInfluence(const RawInfluence* in_influences, size_t in_count, VertexInfluence& out_influence)
    {
        out_influence = VertexInfluence {};

        std::vector<RawInfluence> candidates;
        candidates.reserve(in_count < 16u ? in_count : 16u);
        for (size_t influence = 0; influence < in_count; ++influence)
        {
            if (in_influences[influence].m_BoneIndex < 0 || in_influences[influence].m_Weight <= 0.0f)
            {
                continue;
            }
            candidates.push_back(in_influences[influence]);
        }

        if (candidates.empty())
        {
            return false;
        }

        // Heaviest first; ties broken by bone index so the result is stable
        // across runs (matters for reimport determinism).
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const RawInfluence& in_lhs, const RawInfluence& in_rhs) {
                      if (in_lhs.m_Weight != in_rhs.m_Weight)
                      {
                          return in_lhs.m_Weight > in_rhs.m_Weight;
                      }
                      return in_lhs.m_BoneIndex < in_rhs.m_BoneIndex;
                  });

        const size_t kept = std::min<size_t>(candidates.size(), kMaxInfluences);
        float total = 0.0f;
        for (size_t influence = 0; influence < kept; ++influence)
        {
            out_influence.m_Indices[influence] = candidates[influence].m_BoneIndex + 1;  // +1: slot 0 is the root.
            out_influence.m_Weights[influence] = candidates[influence].m_Weight;
            total += candidates[influence].m_Weight;
        }

        if (total <= kPoseEpsilon)
        {
            out_influence.m_Indices[0] = candidates[0].m_BoneIndex + 1;
            out_influence.m_Weights[0] = 1.0f;
            return true;
        }

        NormalizeInfluence(out_influence);
        return true;
    }

    void NormalizeInfluence(VertexInfluence& in_out_influence)
    {
        float total = 0.0f;
        for (uint32_t influence = 0; influence < kMaxInfluences; ++influence)
        {
            if (in_out_influence.m_Indices[influence] < 0)
            {
                in_out_influence.m_Weights[influence] = 0.0f;
                continue;
            }
            if (in_out_influence.m_Weights[influence] < 0.0f)
            {
                in_out_influence.m_Weights[influence] = 0.0f;
            }
            total += in_out_influence.m_Weights[influence];
        }

        if (total <= kPoseEpsilon)
        {
            // Unbound vertex: pin it to the root so it follows the model instead
            // of collapsing into the origin.
            in_out_influence = VertexInfluence {};
            in_out_influence.m_Indices[0] = 0;
            in_out_influence.m_Weights[0] = 1.0f;
            return;
        }

        const float inverse_total = 1.0f / total;
        for (uint32_t influence = 0; influence < kMaxInfluences; ++influence)
        {
            in_out_influence.m_Weights[influence] *= inverse_total;
        }
    }

    Matrix4x4 BuildBlendMatrix(const VertexInfluence& in_influence, const Matrix4x4* in_palette, size_t in_palette_size)
    {
        Matrix4x4 blend(
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

        bool any_contribution = false;
        for (uint32_t influence = 0; influence < kMaxInfluences; ++influence)
        {
            const int32_t bone = in_influence.m_Indices[influence];
            const float weight = in_influence.m_Weights[influence];
            if (bone < 0 || weight <= 0.0f)
            {
                continue;
            }
            if (static_cast<size_t>(bone) >= in_palette_size)
            {
                continue;  // Out-of-range bone: ignore rather than read garbage.
            }
            blend = blend + in_palette[static_cast<size_t>(bone)] * weight;
            any_contribution = true;
        }

        return any_contribution ? blend : Matrix4x4::IDENTITY;
    }

    namespace
    {
        Vector3 TransformDirection(const Matrix4x4& in_matrix, const Vector3& in_direction)
        {
            const Vector4 transformed = in_matrix * Vector4(in_direction.x, in_direction.y, in_direction.z, 0.0f);
            return Vector3(transformed.x, transformed.y, transformed.z);
        }

        Vector3 TransformPoint(const Matrix4x4& in_matrix, const Vector3& in_point)
        {
            const Vector4 transformed = in_matrix * Vector4(in_point.x, in_point.y, in_point.z, 1.0f);
            return Vector3(transformed.x, transformed.y, transformed.z);
        }
    }  // namespace

    Vector3 SkinPosition(const Vector3& in_bind_position,
                         const VertexInfluence& in_influence,
                         const Matrix4x4* in_palette,
                         size_t in_palette_size)
    {
        return TransformPoint(BuildBlendMatrix(in_influence, in_palette, in_palette_size), in_bind_position);
    }

    Vector3 SkinDirection(const Vector3& in_bind_direction,
                          const VertexInfluence& in_influence,
                          const Matrix4x4* in_palette,
                          size_t in_palette_size)
    {
        const Vector3 transformed = TransformDirection(BuildBlendMatrix(in_influence, in_palette, in_palette_size),
                                                       in_bind_direction);
        const float length = std::sqrt(transformed.x * transformed.x + transformed.y * transformed.y +
                                       transformed.z * transformed.z);
        if (length <= kPoseEpsilon)
        {
            return in_bind_direction;
        }
        const float inverse_length = 1.0f / length;
        return Vector3(transformed.x * inverse_length, transformed.y * inverse_length, transformed.z * inverse_length);
    }

    void SkinVertexBuffer(const Vector3* in_bind_positions,
                          const Vector3* in_bind_normals,
                          const VertexInfluence* in_influences,
                          size_t in_vertex_count,
                          const std::vector<Matrix4x4>& in_palette,
                          Vector3* out_positions,
                          Vector3* out_normals)
    {
        if (in_bind_positions == nullptr || in_influences == nullptr)
        {
            return;
        }

        for (size_t vertex = 0; vertex < in_vertex_count; ++vertex)
        {
            const Matrix4x4 blend = BuildBlendMatrix(in_influences[vertex], in_palette.data(), in_palette.size());
            if (out_positions != nullptr)
            {
                out_positions[vertex] = TransformPoint(blend, in_bind_positions[vertex]);
            }
            if (out_normals != nullptr && in_bind_normals != nullptr)
            {
                Vector3 normal = TransformDirection(blend, in_bind_normals[vertex]);
                const float length =
                    std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length > kPoseEpsilon)
                {
                    normal = normal * (1.0f / length);
                }
                out_normals[vertex] = normal;
            }
        }
    }
}  // namespace AnimationSkinning
