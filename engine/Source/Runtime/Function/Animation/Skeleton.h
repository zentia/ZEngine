#pragma once

#include "Runtime/Function/Animation/Node.h"
#include "Runtime/Resource/ResType/Components/Animation.h"

class SkeletonData;
class BlendStateWithClipData;

class Skeleton
{
private:
    bool m_IsFlat {false};
    int m_BoneCount {0};
    Bone* m_Bones {nullptr};

public:
    ~Skeleton();

    void BuildSkeleton(SkeletonData& skeleton_definition);
    void ApplyAnimation(const BlendStateWithClipData& blend_state);
    AnimationResult OutputAnimationResult();
    void ResetSkeleton();
    const Bone* GetBones() const;
    int32_t GetBonesCount() const;
};