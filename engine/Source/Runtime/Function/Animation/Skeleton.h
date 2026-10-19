#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Animation/Node.h"
#include "Runtime/Resource/ResType/Components/Animation.h"

#include <vector>

class SkeletonData;
class BlendStateWithClipData;

class Skeleton
{
private:
    bool m_IsFlat {false};
    int m_BoneCount {0};
    Bone* m_Bones {nullptr};

    /// Topologically ordered COPY of the asset's bones. We never mutate the
    /// cached `SkeletonData` (AnimationSystem hands the same instance to every
    /// component), and `Bone::Initialize()` keeps a raw `RawBone*` into this
    /// array, so it must outlive `m_Bones` and never reallocate behind its back.
    std::vector<RawBone> m_BoneDefinitions;

    /// inverse(global bind matrix) per bone, derived from `binding_pose`.
    /// `RawBone::tpose_matrix` is deliberately NOT used: nothing in the codebase
    /// writes it, so trusting it silently disables skinning.
    std::vector<Matrix4x4> m_InverseBindMatrices;

public:
    ~Skeleton();

    void BuildSkeleton(SkeletonData& skeleton_definition);
    void ApplyAnimation(const BlendStateWithClipData& blend_state);
    AnimationResult OutputAnimationResult();
    void ResetSkeleton();
    const Bone* GetBones() const;
    int32_t GetBonesCount() const;

    /// Bind-pose local TRS of a bone, in the form the skinning core uses.
    static void MakeBindPose(const RawBone& in_bone, Vector3& out_position, Quaternion& out_rotation, Vector3& out_scale);
};
