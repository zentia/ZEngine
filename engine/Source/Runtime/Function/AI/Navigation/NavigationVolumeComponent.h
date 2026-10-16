#pragma once

// Marks a region of the level the navigation mesh should cover and carries
// the authored build settings.
//
// Scripts/blueprints place and size this volume; they do not trigger the
// build. Rebuilding is driven from the editor or level load so a stray script
// cannot stall the frame with a full re-sample.

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/AI/Navigation/NavMesh.h"
#include "Runtime/Function/Framework/Component/Component.h"

class NavMeshVolumeComponent : public Component
{
    REGISTER_CLASS(NavMeshVolumeComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    NavMeshVolumeComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;

    /// Assemble build settings from the authored fields and the object's
    /// world transform (so moving the volume in the editor moves the mesh).
    NavMeshBuildSettings BuildSettings() const;

    /// Ask NavigationSystem to (re)sample this volume.
    /// @return: walkable cell count, 0 on failure
    int RebuildNavMesh();

    // ---- authored settings (exposed to scripts like any other field) -------

    float GetCellSize() const { return m_CellSize; }
    void SetCellSize(float cell_size) { m_CellSize = cell_size; }

    int GetCellsX() const { return m_CellsX; }
    void SetCellsX(int cells_x) { m_CellsX = cells_x; }

  int GetCellsY() const { return m_CellsY; }
  void SetCellsY(int cells_y) { m_CellsY = cells_y; }

    float GetMaxSlope() const { return m_MaxSlope; }
    void SetMaxSlope(float max_slope) { m_MaxSlope = max_slope; }

    float GetAgentRadius() const { return m_AgentRadius; }
    void SetAgentRadius(float agent_radius) { m_AgentRadius = agent_radius; }

    float GetMaxStepHeight() const { return m_MaxStepHeight; }
    void SetMaxStepHeight(float max_step_height) { m_MaxStepHeight = max_step_height; }

    /// Rebuild automatically when the level loads. Off for volumes that are
    /// only meant to be built on demand.
    bool IsBuildOnLoad() const { return m_BuildOnLoad; }
    void SetBuildOnLoad(bool build_on_load) { m_BuildOnLoad = build_on_load; }

private:
  float m_CellSize {1.f};
  int m_CellsX {32};
  int m_CellsY {32};
    float m_MaxSlope {0.7f};
    float m_AgentRadius {0.4f};
    float m_MaxStepHeight {0.6f};
    bool m_BuildOnLoad {true};
};
