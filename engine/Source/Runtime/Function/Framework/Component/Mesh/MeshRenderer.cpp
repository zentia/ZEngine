#include "MeshRenderer.h"

#include "Runtime/Function/Framework/Component/Transform/Transform.h"



GameObjectDesc MeshRenderer::BuildGameObjectDesc(const Transform* transform_component) const
{
    return BuildGameObjectDescFromParts(BuildRenderParts(transform_component));
}
