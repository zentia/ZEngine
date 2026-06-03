#include "SkinMeshRenderer.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"

IMPLEMENT_REGISTER_CLASS(SkinMeshRenderer)
IMPLEMENT_OBJECT_SERIALIZE(SkinMeshRenderer)

template<typename TransferFunction>
void SkinMeshRenderer::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SkinMeshRenderer)

namespace
{
    SkeletonAnimationResult buildAnimationResult(const AnimationComponent* animation_component)
    {
        SkeletonAnimationResult animation_result;
        animation_result.m_Transforms.push_back({Matrix4x4::IDENTITY});
        if (animation_component != nullptr)
        {
            for (const auto& node : animation_component->GetResult().node)
            {
                animation_result.m_Transforms.push_back({Matrix4x4(node.transform)});
            }
        }
        return animation_result;
    }
}  // namespace

GameObjectDesc SkinMeshRenderer::BuildGameObjectDesc(const TransformComponent* transform_component) const
{
    std::vector<GameObjectPartDesc> render_parts = BuildRenderParts(transform_component);
    const AnimationComponent* animation_component = m_ParentObject != nullptr ? m_ParentObject->tryGetComponentConst(AnimationComponent) : nullptr;

    if (animation_component != nullptr)
    {
        const SkeletonAnimationResult animation_result = buildAnimationResult(animation_component);
        for (GameObjectPartDesc& render_part : render_parts)
        {
            render_part.m_WithAnimation = true;
            render_part.m_SkeletonAnimationResult = animation_result;
            render_part.m_SkeletonBindingDesc.m_SkeletonBindingFile = render_part.m_MeshDesc.m_MeshAsset;
        }
    }

    return BuildGameObjectDescFromParts(render_parts);
}

void SkinMeshRenderer::Tick(float delta_time)
{
    if (m_ParentObject == nullptr)
    {
        return;
    }

    const AnimationComponent* animation_component = m_ParentObject->tryGetComponentConst(AnimationComponent);
    if (animation_component == nullptr)
    {
        BaseRenderer::Tick(delta_time);
        return;
    }

    TransformComponent* transform_component = m_ParentObject->tryGetComponent(TransformComponent);
    SubmitRenderState();
    if (transform_component != nullptr)
    {
        transform_component->setDirtyFlag(false);
    }
}
