#include "Runtime/Function/AI/BehaviourTree.h"

#include "Runtime/Core/Base/Macro.h"

namespace
{
  // A child pointer that is null is a construction bug (a composite with a
  // dangling slot). We report it as failure rather than crashing so a
  // half-built tree in the editor degrades gracefully.
  BTNodeStatus TickChildOrFail(BTNode* child, const BTNodeContext& context, const char* owner_name)
  {
    if (child == nullptr)
    {
      LOG_ERROR(ZGame, "AI: node '{}' has a null child slot", owner_name);
      return BTNodeStatus::failure;
    }
    return child->Tick(context);
  }
}  // namespace

// ============================================================
// Selector
// ============================================================

BTNodeStatus BTSelector::Tick(const BTNodeContext& context)
{
  for (BTNode* child : m_Children)
  {
    const BTNodeStatus status = TickChildOrFail(child, context, m_Name.c_str());
    if (status == BTNodeStatus::success || status == BTNodeStatus::running)
    {
      return status;
    }
  }
  return BTNodeStatus::failure;
}

void BTSelector::Reset()
{
  for (BTNode* child : m_Children)
  {
    if (child != nullptr)
    {
      child->Reset();
    }
  }
}

// ============================================================
// Sequence
// ============================================================

BTNodeStatus BTSequence::Tick(const BTNodeContext& context)
{
  for (BTNode* child : m_Children)
  {
    const BTNodeStatus status = TickChildOrFail(child, context, m_Name.c_str());
    if (status == BTNodeStatus::failure || status == BTNodeStatus::running)
    {
      return status;
    }
  }
  return BTNodeStatus::success;
}

void BTSequence::Reset()
{
  for (BTNode* child : m_Children)
  {
    if (child != nullptr)
    {
      child->Reset();
    }
  }
}

// ============================================================
// Parallel
// ============================================================

BTNodeStatus BTParallel::Tick(const BTNodeContext& context)
{
  if (m_Children.empty())
  {
    return BTNodeStatus::failure;
  }

  uint32_t success_count = 0;
  uint32_t failure_count = 0;
  bool any_running = false;

  for (BTNode* child : m_Children)
  {
    const BTNodeStatus status = TickChildOrFail(child, context, m_Name.c_str());
    switch (status)
    {
      case BTNodeStatus::success:
        ++success_count;
        break;
      case BTNodeStatus::failure:
        ++failure_count;
        break;
      case BTNodeStatus::running:
        any_running = true;
        break;
      case BTNodeStatus::invalid:
      default:
        break;
    }
  }

  const uint32_t failure_threshold = (m_FailureThreshold == 0) ? 1u : m_FailureThreshold;
  if (failure_count >= failure_threshold)
  {
    return BTNodeStatus::failure;
  }

  const uint32_t success_threshold =
      (m_SuccessThreshold == 0) ? static_cast<uint32_t>(m_Children.size()) : m_SuccessThreshold;
  if (success_count >= success_threshold)
  {
    return BTNodeStatus::success;
  }

  return any_running ? BTNodeStatus::running : BTNodeStatus::failure;
}

void BTParallel::Reset()
{
  for (BTNode* child : m_Children)
  {
    if (child != nullptr)
    {
      child->Reset();
    }
  }
}

// ============================================================
// Inverter
// ============================================================

BTNodeStatus BTInverter::Tick(const BTNodeContext& context)
{
  const BTNodeStatus status = TickChildOrFail(m_Child, context, m_Name.c_str());
  if (status == BTNodeStatus::success)
  {
    return BTNodeStatus::failure;
  }
  if (status == BTNodeStatus::failure)
  {
    return BTNodeStatus::success;
  }
  return status;
}

void BTInverter::Reset()
{
  if (m_Child != nullptr)
  {
    m_Child->Reset();
  }
}

// ============================================================
// Repeater
// ============================================================

BTNodeStatus BTRepeater::Tick(const BTNodeContext& context)
{
  if (m_RepeatCount >= 0 && m_Remaining <= 0)
  {
    return BTNodeStatus::success;
  }

  const BTNodeStatus status = TickChildOrFail(m_Child, context, m_Name.c_str());
  if (status == BTNodeStatus::running)
  {
    return BTNodeStatus::running;
  }

  if (m_RepeatCount >= 0)
  {
    --m_Remaining;
    if (m_Remaining <= 0)
    {
      return BTNodeStatus::success;
    }
  }

  // Infinite repeaters swallow child failure and keep the branch alive.
  return BTNodeStatus::running;
}

void BTRepeater::Reset()
{
  m_Remaining = m_RepeatCount;
  if (m_Child != nullptr)
  {
    m_Child->Reset();
  }
}

// ============================================================
// Cooldown
// ============================================================

BTNodeStatus BTCooldown::Tick(const BTNodeContext& context)
{
  if (m_Remaining > 0.f)
  {
    m_Remaining -= context.delta_time;
    return BTNodeStatus::failure;
  }

  const BTNodeStatus status = TickChildOrFail(m_Child, context, m_Name.c_str());
  if (status != BTNodeStatus::running)
  {
    m_Remaining = m_CooldownSeconds;
  }
  return status;
}

void BTCooldown::Reset()
{
  m_Remaining = 0.f;
  if (m_Child != nullptr)
  {
    m_Child->Reset();
  }
}

// ============================================================
// Leaves
// ============================================================

BTNodeStatus BTCondition::Tick(const BTNodeContext& context)
{
  if (m_Predicate == nullptr)
  {
    LOG_ERROR(ZGame, "AI: condition '{}' has no predicate", m_Name.c_str());
    return BTNodeStatus::failure;
  }
  if (context.blackboard == nullptr)
  {
    return BTNodeStatus::failure;
  }
  return m_Predicate(*context.blackboard) ? BTNodeStatus::success : BTNodeStatus::failure;
}

BTNodeStatus BTAction::Tick(const BTNodeContext& context)
{
  if (m_Functor == nullptr)
  {
    LOG_ERROR(ZGame, "AI: action '{}' has no functor", m_Name.c_str());
    return BTNodeStatus::failure;
  }
  if (context.blackboard == nullptr)
  {
    return BTNodeStatus::failure;
  }
  return m_Functor(*context.blackboard, context.delta_time);
}

// ============================================================
// Tree
// ============================================================

BTNodeStatus BehaviourTree::Tick(AIBlackboard& blackboard, float delta_time)
{
  if (m_Root == nullptr)
  {
    m_LastStatus = BTNodeStatus::failure;
    return m_LastStatus;
  }

  m_ElapsedTime += delta_time;

  BTNodeContext context;
  context.blackboard = &blackboard;
  context.delta_time = delta_time;
  context.elapsed_time = m_ElapsedTime;

  m_LastStatus = m_Root->Tick(context);
  return m_LastStatus;
}

void BehaviourTree::Reset()
{
  if (m_Root != nullptr)
  {
    m_Root->Reset();
  }
  m_LastStatus = BTNodeStatus::invalid;
  m_ElapsedTime = 0.f;
}

void BehaviourTree::Clear()
{
  for (BTNode* node : m_Nodes)
  {
    delete node;
  }
  m_Nodes.clear();
  m_Root = nullptr;
  m_LastStatus = BTNodeStatus::invalid;
  m_ElapsedTime = 0.f;
}
