#include "Runtime/Function/Animation/Skeleton.h"

#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Animation/Utilities.h"

Skeleton::~Skeleton()
{
    delete[] m_Bones;
}

void Skeleton::ResetSkeleton()
{
    for (size_t i = 0; i < m_BoneCount; i++)
        m_Bones[i].ResetToInitialPose();
}

void Skeleton::BuildSkeleton(SkeletonData& skeleton_definition)
{
    m_IsFlat = skeleton_definition.is_flat;
    if (m_Bones != nullptr)
    {
        delete[] m_Bones;
    }
    if (!m_IsFlat || !skeleton_definition.in_topological_order)
    {
        // LOG_ERROR
        return;
    }
    m_BoneCount = skeleton_definition.bones_map.size();
    m_Bones = new Bone[m_BoneCount];
    for (size_t i = 0; i < m_BoneCount; i++)
    {
        RawBone& bone_definition = skeleton_definition.bones_map[i];
        Bone* parent_bone = find_by_index(m_Bones, bone_definition.parent_index, i, m_IsFlat);
        m_Bones[i].Initialize(&bone_definition, parent_bone);
    }
}

void Skeleton::ApplyAnimation(const BlendStateWithClipData& blend_state)
{
    if (!m_Bones)
    {
        return;
    }
    ResetSkeleton();
    for (size_t clip_index = 0; clip_index < 1; clip_index++)
    {
        const AnimationClip& animation_clip = blend_state.blend_clip[clip_index];
        const float phase = blend_state.blend_ratio[clip_index];
        const AnimSkelMap* anim_skel_map = blend_state.blend_anim_skel_map[clip_index];

        float exact_frame = phase * (animation_clip.total_frame - 1);
        int current_frame_low = floor(exact_frame);
        int current_frame_high = ceil(exact_frame);
        float lerp_ratio = exact_frame - current_frame_low;
        // for (size_t node_index = 0; node_index < 0; node_index++)
        for (size_t node_index = 0; node_index < animation_clip.node_count && node_index < anim_skel_map->convert.size();
             node_index++)
        {
            AnimationChannel channel = animation_clip.node_channels[node_index];
            size_t bone_index = anim_skel_map->convert[node_index];
            float weight = 1;  // blend_state.blend_weight[clip_index]->blend_weight[bone_index];
            weight = 1;
            if (fabs(weight) < 0.0001f)
            {
                continue;
            }
            if (bone_index == std::numeric_limits<size_t>().max())
            {
                // LOG_WARNING
                continue;
            }
            Bone* bone = &m_Bones[bone_index];
            if (channel.position_keys.size() <= current_frame_high)
            {
                current_frame_high = channel.position_keys.size() - 1;
            }
            if (channel.scaling_keys.size() <= current_frame_high)
            {
                current_frame_high = channel.scaling_keys.size() - 1;
            }
            if (channel.rotation_keys.size() <= current_frame_high)
            {
                current_frame_high = channel.rotation_keys.size() - 1;
            }
            current_frame_low = (current_frame_low < current_frame_high) ? current_frame_low : current_frame_high;
            Vector3 position = Vector3::Lerp(
                channel.position_keys[current_frame_low], channel.position_keys[current_frame_high], lerp_ratio);
            Vector3 scaling = Vector3::Lerp(
                channel.scaling_keys[current_frame_low], channel.scaling_keys[current_frame_high], lerp_ratio);
            Quaternion rotation = Quaternion::NLerp(
                lerp_ratio, channel.rotation_keys[current_frame_low], channel.rotation_keys[current_frame_high], true);

            {
                bone->Rotate(rotation);
                bone->Scale(scaling);
                bone->Translate(position);

                // bone->Rotate({ {},0.01,0,0,1 });
                // bone->Scale({ {},0.9,0.9,0.9 });
            }
        }
    }
    // bones[77].Rotate(Quaternion{ {},1,0,0,1 });
    // bones[18].Translate(Vector3{ {},0,1,0 });
    // bones[0].SetScale(Vector3{ {},0,1,0.01 });
    for (size_t i = 0; i < m_BoneCount; i++)
    {
        m_Bones[i].Update();
    }
#ifdef _DEBUG
    // bones[107].m_DerivedPosition += Vector3{ {},10, 0, 0 };
#endif
}

AnimationResult Skeleton::OutputAnimationResult()
{
    AnimationResult animation_result;
    for (size_t i = 0; i < m_BoneCount; i++)
    {
        std::shared_ptr<AnimationResultElement> animation_result_element = std::make_shared<AnimationResultElement>();
        Bone* bone = &m_Bones[i];
        animation_result_element->index = bone->GetID() + 1;
        Vector3 temp_translation = bone->_getDerivedPosition();

        // TODO: the unit of the joint matrices is wrong
        Vector3 temp_scale = bone->_getDerivedScale();

        Quaternion temp_rotation = bone->_getDerivedOrientation();

        // auto scale = bone->_getDerivedTScale();
        // scale.x = 1.f / scale.x;
        // scale.y = 1.f / scale.y;
        // scale.z = 1.f / scale.z;

        // auto revTmat = getMatrix(
        //	-bone->_getDerivedTPosition(),
        //	scale,
        //	conjugate( bone->_getDerivedTOrientation())
        //);
        auto objMat = Transform(bone->_getDerivedPosition(), bone->_getDerivedOrientation(), bone->_getDerivedScale())
                          .getMatrix();

        auto resMat = objMat * bone->_getInverseTpose();

        animation_result_element->transform = resMat.toMatrix4x4_();

        animation_result.node.push_back(*animation_result_element);
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