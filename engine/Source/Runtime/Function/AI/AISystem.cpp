#include "Runtime/Function/AI/AISystem.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include <algorithm>

std::vector<std::type_index> AISystem::GetDependencies() const
{
  // Perception reads the level graph and the physics scene through
  // WorldManager, so the world must exist before any agent ticks.
  return {std::type_index(typeid(WorldManager))};
}

bool AISystem::Initialize()
{
  m_IsEnabled = true;
  m_RemainingBudgetMs = m_FrameBudgetMs;
  ResetStats();

  LOG_INFO(ZGame, "AI: AISystem initialized (budget {} ms/frame)", m_FrameBudgetMs);
  return true;
}

void AISystem::Shutdown()
{
  ResetStats();
  LOG_INFO(ZGame, "AI: AISystem shut down");
}

void AISystem::BeginFrame(float delta_time)
{
  (void)delta_time;

  m_RemainingBudgetMs = m_FrameBudgetMs;
  m_SpentThisFrameMs = 0.f;
  m_LastFrameSpendMs = 0.f;
  m_TickedAgentCount = 0;
}

void AISystem::ReportAgentSpend(float milliseconds)
{
  m_SpentThisFrameMs += milliseconds;
  m_TickedAgentCount += 1;

  if (m_SpentThisFrameMs > m_FrameBudgetMs)
  {
    m_BudgetExceededCount += 1;
  }

  m_RemainingBudgetMs = std::max(0.f, m_FrameBudgetMs - m_SpentThisFrameMs);
  m_LastFrameSpendMs = m_SpentThisFrameMs;
}

void AISystem::ResetStats()
{
  m_LastFrameSpendMs = 0.f;
  m_SpentThisFrameMs = 0.f;
  m_TickedAgentCount = 0;
  m_BudgetExceededCount = 0;
}
