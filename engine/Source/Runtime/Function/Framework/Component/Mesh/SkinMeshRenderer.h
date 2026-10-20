#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "SkinMeshRenderer.generated.h"

class AnimationComponent;

ZCLASS()
class SkinMeshRenderer : public BaseRenderer
{
    ZGENERATED_BODY(SkinMeshRenderer)

public:
    SkinMeshRenderer() = default;

    void Tick(float delta_time) override;

private:
    GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const override;
};
