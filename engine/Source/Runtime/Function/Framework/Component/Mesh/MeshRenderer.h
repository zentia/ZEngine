#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "MeshRenderer.generated.h"

ZCLASS()
class MeshRenderer : public BaseRenderer
{
    ZGENERATED_BODY(MeshRenderer)

public:
    MeshRenderer() = default;

private:
    GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const override;
};
