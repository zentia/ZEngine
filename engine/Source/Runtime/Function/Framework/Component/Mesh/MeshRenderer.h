#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"

class MeshRenderer : public BaseRenderer
{
    REGISTER_CLASS(MeshRenderer);
    DECLARE_OBJECT_SERIALIZE();

public:
    MeshRenderer() = default;

private:
    GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const override;
};
