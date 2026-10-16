#include "Runtime/Function/AI/Navigation/NavigationVolumeComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

IMPLEMENT_REGISTER_CLASS(NavMeshVolumeComponent);
IMPLEMENT_OBJECT_SERIALIZE(NavMeshVolumeComponent);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(NavMeshVolumeComponent);

template <typename TransferFunction>
void NavMeshVolumeComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_CellSize, "cell_size");
    transfer.Transfer(m_CellsX, "cells_x");
    transfer.Transfer(m_CellsY, "cells_y");
    transfer.Transfer(m_MaxSlope, "max_slope");
    transfer.Transfer(m_AgentRadius, "agent_radius");
    transfer.Transfer(m_MaxStepHeight, "max_step_height");
    transfer.Transfer(m_BuildOnLoad, "build_on_load");
}

void NavMeshVolumeComponent::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);

    if (!m_BuildOnLoad)
    {
        return;
    }

    RebuildNavMesh();
}

void NavMeshVolumeComponent::OnSerializedFieldsUpdated()
{
    // Editing a field in the inspector should not silently leave a stale mesh:
    // rebuild so what is drawn matches what is authored.
    if (m_BuildOnLoad && m_ParentObject != nullptr)
    {
        RebuildNavMesh();
    }
}

NavMeshBuildSettings NavMeshVolumeComponent::BuildSettings() const
{
    NavMeshBuildSettings settings;

    // Origin tracks the object's world transform so the volume can be moved
    // in the editor without re-authoring the grid.
    settings.origin = Vector3::ZERO;
    if (m_ParentObject != nullptr)
    {
        if (Transform* transform = m_ParentObject->tryGetComponent<Transform>("Transform"))
        {
            settings.origin = transform->GetLocalPosition();
        }
    }

    settings.cell_size = m_CellSize;
    settings.cells_x = m_CellsX;
    settings.cells_y = m_CellsY;
    settings.max_slope = m_MaxSlope;
    settings.agent_radius = m_AgentRadius;
    settings.max_step_height = m_MaxStepHeight;

    return settings;
}

int NavMeshVolumeComponent::RebuildNavMesh()
{
    NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
    if (navigation_system == nullptr)
    {
        LOG_ERROR(ZGame, "NavMeshVolumeComponent: NavigationSystem unavailable");
        return 0;
    }

    const int walkable = navigation_system->BuildFromVolume(BuildSettings());
    if (walkable == 0)
    {
        LOG_WARNING(ZGame, "NavMeshVolumeComponent: rebuild produced no walkable cells");
    }

    return walkable;
}
