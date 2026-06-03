#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"

class AnimationComponent;

class SkinMeshRenderer : public BaseRenderer
{
    REGISTER_CLASS(SkinMeshRenderer);
    DECLARE_OBJECT_SERIALIZE();

public:
    SkinMeshRenderer() = default;

    void Tick(float delta_time) override;

private:
    GameObjectDesc BuildGameObjectDesc(const TransformComponent* transform_component) const override;
};
