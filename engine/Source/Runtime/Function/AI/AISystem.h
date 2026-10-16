#pragma once

// Global bookkeeping for the AI layer.
//
// Phase 1 deliberately keeps this thin: it owns no trees and ticks no agents.
// AIControllerComponent does its own ticking because the Component::Tick path
// is already driven by Level::Tick and respects per-object enable flags.
// AISystem exists to give the layer one registered place for:
//   - a global enable switch (kill switch for profiling / bug repro),
//   - per-frame budgeting across all agents,
//   - stats the editor and ztrace panels read.
//
// Later phases move perception updates and async pathfinding onto the
// TaskGraph inside this system; the public API below is shaped so callers do
// not have to change when that happens.

#include "Runtime/Core/Base/EngineSystem.h"

#include <cstdint>

class AISystem : public IEngineSystem
{
public:
  std::string GetName() const override { return "AISystem"; }
  SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }
  std::vector<std::type_index> GetDependencies() const override;

  bool Initialize() override;
  void Shutdown() override;

  /// Frame boundary for the AI budget. Called once per frame by Application
  /// before levels tick; agents consult GetRemainingBudgetMs().
  void BeginFrame(float delta_time);

  /// Global kill switch. When disabled, every AIControllerComponent short
  /// circuits without touching its tree.
  bool IsAIEnabled() const { return m_IsEnabled; }
  void SetAIEnabled(bool enabled) { m_IsEnabled = enabled; }

  /// Milliseconds an agent may still spend this frame.
  float GetRemainingBudgetMs() const { return m_RemainingBudgetMs; }
  float GetFrameBudgetMs() const { return m_FrameBudgetMs; }
  void SetFrameBudgetMs(float budget_ms) { m_FrameBudgetMs = budget_ms; }

  /// Report actual spend; drives the stats below.
  void ReportAgentSpend(float milliseconds);

  // ---- stats (read by editor overlays / ztrace) -----------------------------

  uint32_t GetTickedAgentCount() const { return m_TickedAgentCount; }
  float GetLastFrameSpendMs() const { return m_LastFrameSpendMs; }
  uint32_t GetBudgetExceededCount() const { return m_BudgetExceededCount; }

  void ResetStats();

private:
  bool m_IsEnabled {true};

  float m_FrameBudgetMs {2.f};
  float m_RemainingBudgetMs {0.f};
  float m_LastFrameSpendMs {0.f};
  float m_SpentThisFrameMs {0.f};

  uint32_t m_TickedAgentCount {0};
  uint32_t m_BudgetExceededCount {0};
};
