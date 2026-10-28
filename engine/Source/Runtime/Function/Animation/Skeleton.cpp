#include "Runtime/Function/Animation/Skeleton.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Function/Animation/Skinning.h"

#include <algorithm>

Skeleton::~Skeleton()
{
    delete[] m_Bones;
}

void Skeleton::ResetSkeleton()
{
    for (size_t i = 0; i < m_BoneCount; i++)
        m_Bones[i].ResetToInitialPose();
}

void Skeleton::MakeBindPose(const RawBone& in_bone, Vector3& out_position, Quaternion& out_rotation, Vector3& out_scale)
{
    out_position = in_bone.binding_pose.m_Position.ToVector3();
    out_rotation = in_bone.binding_pose.m_Rotation;
    out_scale = in_bone.binding_pose.m_Scale;
}

void Skeleton::BuildSkeleton(SkeletonData& skeleton_definition)
{
    if (m_Bones != nullptr)
    {
        delete[] m_Bones;
        m_Bones = nullptr;
    }
    m_BoneCount = 0;
    m_InverseBindMatrices.clear();

    // Copy before sorting: the definition is shared cached data.
    m_BoneDefinitions = skeleton_definition.bones_map;

    // The old code bailed out silently here unless the asset was already flat
    // AND topologically ordered, which is how "the character never animates"
    // went undiagnosed for months. Sorting is cheap and always safe.
    if (!AnimationSkinning::EnsureTopologicalOrder(m_BoneDefinitions))
    {
        LOG_ERROR(ZAnimation,
                  "Skeleton '{}' has a cyclic bone hierarchy ({} bones); animation disabled for this skeleton",
                  skeleton_definition.bones_map.size() > 0 ? std::string(skeleton_definition.bones_map[0].name.c_str())
                                                           : std::string("<empty>"),
                  skeleton_definition.bones_map.size());
        m_BoneDefinitions.clear();
        return;
    }

    m_IsFlat = true;  // EnsureTopologicalOrder renumbers flat: slot == index.
    m_BoneCount = static_cast<int>(m_BoneDefinitions.size());

    AnimationSkinning::ComputeInverseBindMatrices(m_BoneDefinitions, m_InverseBindMatrices);

    m_Bones = new Bone[m_BoneCount];
    for (int i = 0; i < m_BoneCount; i++)
    {
        RawBone& bone_definition = m_BoneDefinitions[static_cast<size_t>(i)];
        // Topological order guarantees the parent is already initialised.
        Bone* parent_bone =
            (bone_definition.parent_index >= 0 && bone_definition.parent_index < i) ? &m_Bones[bone_definition.parent_index]
                                                                                   : nullptr;
        m_Bones[i].Initialize(&bone_definition, parent_bone);
    }
}

void Skeleton::ApplyAnimation(const BlendStateWithClipData& blend_state)
{
    if (!m_Bones)
    {
        return;
    }

    // Reset first: every channel in this engine is a DELTA on the bind pose, so
    // accumulating onto a pose left over from the previous frame compounds it.
    ResetSkeleton();

    const size_t bone_count = static_cast<size_t>(m_BoneCount);
    std::vector<AnimationSkinning::BonePose> accumulator(bone_count, AnimationSkinning::BonePose::Identity());
    std::vector<float> total_weight(bone_count, 0.0f);

    const size_t clip_count = std::min({static_cast<size_t>(std::max(blend_state.clip_count, 0)),
                                        blend_state.blend_clip.size(),
                                        blend_state.blend_anim_skel_map.size()});

    for (size_t clip_index = 0; clip_index < clip_count; clip_index++)
    {
        // The skeleton map is a PPtr and can legitimately resolve to null
        // (asset still loading, or missing on disk). Dereferencing it here
        // crashes the whole frame, so skip this clip instead.
        const AnimSkelMap* anim_skel_map = blend_state.blend_anim_skel_map[clip_index];
        if (anim_skel_map == nullptr)
        {
            continue;
        }

        // Borrowed from AnimationSystem's cache (P0.2): the entry is valid for
        // the duration of this Tick. Guarded anyway -- GetBlendStateWithClipData
        // never pushes a null, but ApplyAnimation is public and a hand-built
        // BlendStateWithClipData must not be able to fault here.
        const AnimationClip* animation_clip_ptr = blend_state.blend_clip[clip_index];
        if (animation_clip_ptr == nullptr)
        {
            continue;
        }
        const AnimationClip& animation_clip = *animation_clip_ptr;
        const float phase = (clip_index < blend_state.blend_ratio.size()) ? blend_state.blend_ratio[clip_index] : 0.0f;

        std::vector<AnimationSkinning::BonePose> poses;
        AnimationSkinning::SampleClip(animation_clip, phase, AnimationSkinning::WrapMode::Loop, poses);

        const size_t node_count = std::min(poses.size(), anim_skel_map->convert.size());
        for (size_t node_index = 0; node_index < node_count; node_index++)
        {
            const int bone_index = anim_skel_map->convert[node_index];
            if (bone_index < 0 || static_cast<size_t>(bone_index) >= bone_count)
            {
                continue;
            }

            float weight = 1.0f;
            if (clip_index < blend_state.blend_weight.size())
            {
                const std::vector<float>& bone_weights = blend_state.blend_weight[clip_index].blend_weight;
                if (static_cast<size_t>(bone_index) < bone_weights.size())
                {
                    weight = bone_weights[static_cast<size_t>(bone_index)];
                }
            }

            AnimationSkinning::AccumulateBlend(
                poses[node_index], weight, accumulator[static_cast<size_t>(bone_index)],
                total_weight[static_cast<size_t>(bone_index)]);
        }
    }

    for (size_t bone_index = 0; bone_index < bone_count; bone_index++)
    {
        AnimationSkinning::BonePose delta = accumulator[bone_index];
        AnimationSkinning::NormalizeBlendAccumulator(delta, total_weight[bone_index]);

        Vector3 bind_position;
        Quaternion bind_rotation;
        Vector3 bind_scale;
        MakeBindPose(m_BoneDefinitions[bone_index], bind_position, bind_rotation, bind_scale);

        AnimationSkinning::BonePose bind_pose;
        bind_pose.m_Position = bind_position;
        bind_pose.m_Rotation = bind_rotation;
        bind_pose.m_Scale = bind_scale;

        const AnimationSkinning::BonePose local = AnimationSkinning::ComposeDelta(bind_pose, delta);

        Bone& bone = m_Bones[bone_index];
        bone.SetPosition(local.m_Position);
        bone.SetOrientation(local.m_Rotation);
        bone.SetScale(local.m_Scale);
    }

    for (size_t i = 0; i < bone_count; i++)
    {
        m_Bones[i].Update();
    }
}

AnimationResult Skeleton::OutputAnimationResult()
{
    AnimationResult animation_result;
    if (!m_Bones)
    {
        return animation_result;
    }

    animation_result.node.reserve(static_cast<size_t>(m_BoneCount));
    for (int i = 0; i < m_BoneCount; i++)
    {
        const Bone& bone = m_Bones[i];

        const Matrix4x4 object_matrix =
            TransformTRS(bone._getDerivedPosition(), bone._getDerivedOrientation(), bone._getDerivedScale()).getMatrix();
        const Matrix4x4& inverse_bind =
            (static_cast<size_t>(i) < m_InverseBindMatrices.size()) ? m_InverseBindMatrices[static_cast<size_t>(i)]
                                                                   : Matrix4x4::IDENTITY;

        // Skinning matrix: current global pose brought back through the bind pose.
        Matrix4x4 skinning_matrix = object_matrix;
        skinning_matrix = skinning_matrix * inverse_bind;

        AnimationResultElement element;
        // +1: slot 0 of the GPU palette is the identity root (see Skinning.h).
        element.index = bone.GetID() + 1;
        element.transform = skinning_matrix.toMatrix4x4_();
        animation_result.node.push_back(element);
    }
    return animation_result;
}

const Bone* Skeleton::GetBones() const
{
    return m_Bones;
}

int32_t Skeleton::GetBonesCount() const
{
    return m_BoneCount;
}
