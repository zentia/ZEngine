#pragma once

// Minimal behaviour tree: selector, sequence, parallel, decorator, leaf.
//
// The tree is intentionally *not* an asset yet. Nodes are value types owned by
// the tree vector and referenced by index, which keeps construction trivial and
// avoids a node allocation per tick. Asset-backed trees with hot reload land in
// a later phase; the BTNodeContext contract below is what an asset loader will
// populate, so it stays stable.

#include "Runtime/Function/AI/AIBlackboard.h"
#include "Runtime/Function/AI/AITypes.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cstdint>

class AIBlackboard;

/// Everything a node is allowed to see while being ticked.
struct BTNodeContext
{
  AIBlackboard* blackboard {nullptr};
  float delta_time {0.f};
  /// Seconds accumulated since the tree started, for cooldown / timeout nodes.
  float elapsed_time {0.f};
};

class BTNode
{
public:
  explicit BTNode(eastl::string name) : m_Name(eastl::move(name)) {}
  virtual ~BTNode() = default;

  BTNode(const BTNode&) = delete;
  BTNode& operator=(const BTNode&) = delete;

  /// Advance the node. Implementations must be safe to call every frame and
  /// must not mutate the world directly -- they write intents on the
  /// blackboard instead.
  virtual BTNodeStatus Tick(const BTNodeContext& context) = 0;

  /// Abort a running subtree. Called when a higher-priority branch takes over
  /// or when the owning component is disabled, so nodes can drop transient
  /// state (timers, partially consumed paths).
  virtual void Reset() {}

  const eastl::string& GetName() const { return m_Name; }

  /// Debug/status label used by the editor overlay.
  virtual const char* GetTypeName() const { return "Node"; }

protected:
  eastl::string m_Name;
};

// ============================================================
// Composites
// ============================================================

/// Selector (fallback): ticks children until one succeeds or is running.
/// Returns failure only when every child failed.
class BTSelector final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Selector"; }

  void AddChild(BTNode* child) { m_Children.push_back(child); }
  const eastl::vector<BTNode*>& GetChildren() const { return m_Children; }

private:
  eastl::vector<BTNode*> m_Children;
};

/// Sequence: ticks children in order; stops at the first failure/running.
/// Returns success only when every child succeeded.
class BTSequence final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Sequence"; }

  void AddChild(BTNode* child) { m_Children.push_back(child); }
  const eastl::vector<BTNode*>& GetChildren() const { return m_Children; }

private:
  eastl::vector<BTNode*> m_Children;
};

/// Parallel: ticks every child each frame.
/// success_threshold = how many children must succeed for the node to succeed
/// (0 means "all"). Failure triggers as soon as `failure_threshold` children
/// fail. Used for "move while watching for danger".
class BTParallel final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Parallel"; }

  void AddChild(BTNode* child) { m_Children.push_back(child); }
  const eastl::vector<BTNode*>& GetChildren() const { return m_Children; }

  void SetSuccessThreshold(uint32_t threshold) { m_SuccessThreshold = threshold; }
  void SetFailureThreshold(uint32_t threshold) { m_FailureThreshold = threshold; }

private:
  eastl::vector<BTNode*> m_Children;
  uint32_t m_SuccessThreshold {0};
  uint32_t m_FailureThreshold {1};
};

// ============================================================
// Decorators
// ============================================================

/// Inverts success/failure of its child. Running is passed through.
class BTInverter final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Inverter"; }

  void SetChild(BTNode* child) { m_Child = child; }

private:
  BTNode* m_Child {nullptr};
};

/// Runs its child repeatedly until it fails; returns success when the child
/// fails. `-1` repeats forever (the node then reports running).
class BTRepeater final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Repeater"; }

  void SetChild(BTNode* child) { m_Child = child; }

  /// Negative count repeats forever.
  void SetRepeatCount(int32_t count)
  {
    m_RepeatCount = count;
    m_Remaining = count;
  }

private:
  BTNode* m_Child {nullptr};
  int32_t m_RepeatCount {-1};
  int32_t m_Remaining {-1};
};

/// Gates its child behind a cooldown in seconds; while cooling down the node
/// reports failure without ticking the child.
class BTCooldown final : public BTNode
{
public:
  using BTNode::BTNode;

  BTNodeStatus Tick(const BTNodeContext& context) override;
  void Reset() override;
  const char* GetTypeName() const override { return "Cooldown"; }

  void SetChild(BTNode* child) { m_Child = child; }
  void SetCooldownSeconds(float seconds) { m_CooldownSeconds = seconds; }

private:
  BTNode* m_Child {nullptr};
  float m_CooldownSeconds {0.f};
  float m_Remaining {0.f};
};

// ============================================================
// Leaves
// ============================================================

/// Condition leaf backed by a predicate over the blackboard.
/// The predicate returns a bool and is expected to be cheap and pure.
class BTCondition final : public BTNode
{
public:
  using Predicate = bool (*)(const AIBlackboard& blackboard);

  BTCondition(eastl::string name, Predicate predicate)
    : BTNode(eastl::move(name)), m_Predicate(predicate)
  {
  }

  BTNodeStatus Tick(const BTNodeContext& context) override;
  const char* GetTypeName() const override { return "Condition"; }

private:
  Predicate m_Predicate {nullptr};
};

/// Action leaf that writes an outcome on the blackboard.
/// Actions must never touch physics or transforms directly: they publish
/// AIMoveIntent / AIActionRequest and return their own status.
class BTAction final : public BTNode
{
public:
  using Functor = BTNodeStatus (*)(AIBlackboard& blackboard, float delta_time);

  BTAction(eastl::string name, Functor functor) : BTNode(eastl::move(name)), m_Functor(functor) {}

  BTNodeStatus Tick(const BTNodeContext& context) override;
  const char* GetTypeName() const override { return "Action"; }

private:
  Functor m_Functor {nullptr};
};

// ============================================================
// Tree
// ============================================================

/// Owns every node of one tree and ticks the root.
class BehaviourTree
{
public:
  ~BehaviourTree() { Clear(); }

  BTNodeStatus Tick(AIBlackboard& blackboard, float delta_time);

  /// Abort the tree and drop all transient node state.
  void Reset();

  /// Transfer ownership of a node to the tree. Returns the raw pointer that
  /// composites should use as a child reference.
  template <typename TNode, typename... TArgs>
  TNode* CreateNode(TArgs&&... args)
  {
    TNode* node = new TNode(static_cast<TArgs&&>(args)...);
    m_Nodes.push_back(node);
    return node;
  }

  void SetRoot(BTNode* root) { m_Root = root; }
  BTNode* GetRoot() const { return m_Root; }

  /// Last status returned by Tick; invalid before the first tick.
  BTNodeStatus GetLastStatus() const { return m_LastStatus; }

  void Clear();

private:
  eastl::vector<BTNode*> m_Nodes;
  BTNode* m_Root {nullptr};
  BTNodeStatus m_LastStatus {BTNodeStatus::invalid};
  float m_ElapsedTime {0.f};
};
