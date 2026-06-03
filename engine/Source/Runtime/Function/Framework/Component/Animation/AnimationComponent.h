#pragma once

#include "Runtime/Function/Animation/Skeleton.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Animation.h"

class AnimationComponent : public Component
{
public:
    AnimationComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;

    void Tick(float delta_time) override;

    const AnimationResult& GetResult() const;

    const Skeleton& GetSkeleton() const;

protected:
    AnimationComponentRes m_AnimationRes;

    Skeleton m_Skeleton;
};