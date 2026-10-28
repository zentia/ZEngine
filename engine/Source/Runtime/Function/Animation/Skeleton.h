#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Animation/Node.h"
#include "Runtime/Function/Animation/Skinning.h"
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

    // ---- per-frame scratch (P0.2) ------------------------------------------
    //
    // ApplyAnimation runs once per animating object per frame and used to
    // declare these as locals, i.e. 2 + (one per clip) heap allocations every
    // frame. Held as members so the buffers survive; every one of them is
    // assign()-ed at the top of its use, so nothing leaks between frames.
    //
    // Not serialized and not part of the skeleton's identity -- BuildSkeleton
    // leaves them alone.
    std::vector<AnimationSkinning::BonePose> m_AccumulatorScratch;
    std::vector<float> m_TotalWeightScratch;
    std::vector<AnimationSkinning::BonePose> m_PoseScratch;

public:
    ~Skeleton();

    void BuildSkeleton(SkeletonData& skeleton_definition);
    void ApplyAnimation(const BlendStateWithClipData& blend_state);

    /// Fills `out_result` with one skinning matrix per bone. Takes an out
    /// parameter rather than returning by value (P0.2) because the caller holds
    /// the result across frames: returning a fresh AnimationResult reallocated
    /// its vector on every Tick.
    void OutputAnimationResult(AnimationResult& out_result);
    void ResetSkeleton();
    const Bone* GetBones() const;
    int32_t GetBonesCount() const;

    /// Bind-pose local TRS of a bone, in the form the skinning core uses.
    static void MakeBindPose(const RawBone& in_bone, Vector3& out_position, Quaternion& out_rotation, Vector3& out_scale);
};
