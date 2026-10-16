// Headless smoke test for the AI layer: blackboard + behaviour tree, no engine
// systems, no physics, no level.
//
// Mirrors the repo's existing smoke-test convention: this TU owns its own
// main() and is carved out of the ZRuntime glob in
// engine/Source/Runtime/CMakeLists.txt so /WHOLEARCHIVE never drags a second
// main() into ZEditor / ZLauncher.

#include "Runtime/Function/AI/AIBlackboard.h"
#include "Runtime/Function/AI/AITypes.h"
#include "Runtime/Function/AI/BehaviourTree.h"

#include <EASTL/string.h>

#include <cstdio>

namespace
{
  int g_failures = 0;

  void Check(bool condition, const char* what)
  {
    if (condition)
    {
      std::printf("[ ok ] %s\n", what);
      return;
    }
    std::printf("[FAIL] %s\n", what);
    ++g_failures;
  }

  // ---- tree fixtures --------------------------------------------------------

  bool AlwaysTrue(const AIBlackboard& blackboard)
  {
    (void)blackboard;
    return true;
  }

  bool AlwaysFalse(const AIBlackboard& blackboard)
  {
    (void)blackboard;
    return false;
  }

  bool FlagSet(const AIBlackboard& blackboard) { return blackboard.GetBoolOrDefault("flag", false); }

  BTNodeStatus SetFlag(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    blackboard.SetBool("flag", true);
    return BTNodeStatus::success;
  }

  BTNodeStatus MoveSomewhere(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    AIMoveIntent intent;
    intent.has_move_target = true;
    intent.move_target = Vector3(1.f, 0.f, 0.f);
    blackboard.SetMoveIntent(intent);
    return BTNodeStatus::running;
  }
}  // namespace

int main()
{
  // ---- blackboard slots -----------------------------------------------------
  {
    AIBlackboard blackboard;
    float missing_read = 0.f;
    Check(!blackboard.TryGetFloat("missing", missing_read), "missing float lookup fails");
    Check(missing_read == 0.f, "failed lookup leaves the out-param untouched");

    float read_back = 0.f;
    blackboard.SetFloat("speed", 3.5f);
    Check(blackboard.TryGetFloat("speed", read_back) && read_back == 3.5f, "float round-trips");
    Check(blackboard.GetFloatOrDefault("missing", 9.f) == 9.f, "float default applies");

    blackboard.SetString("action", eastl::string("attack"));
    eastl::string action;
    Check(blackboard.TryGetString("action", action) && action == eastl::string("attack"),
          "string round-trips");

    blackboard.SetBool("flag", true);
    Check(blackboard.GetBoolOrDefault("flag", false), "bool round-trips");
  }

  // ---- selector semantics ---------------------------------------------------
  {
    BehaviourTree tree;
    BTSelector* root = tree.CreateNode<BTSelector>("root");
    root->AddChild(tree.CreateNode<BTCondition>("false", &AlwaysFalse));
    root->AddChild(tree.CreateNode<BTAction>("set_flag", &SetFlag));
    tree.SetRoot(root);

    AIBlackboard blackboard;
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::success, "selector falls through to success");
    Check(blackboard.GetBoolOrDefault("flag", false), "selector executed the second child");

    BehaviourTree all_failing;
    BTSelector* failing_root = all_failing.CreateNode<BTSelector>("root");
    failing_root->AddChild(all_failing.CreateNode<BTCondition>("false", &AlwaysFalse));
    all_failing.SetRoot(failing_root);
    AIBlackboard empty_blackboard;
    Check(all_failing.Tick(empty_blackboard, 0.016f) == BTNodeStatus::failure,
          "selector fails when every child fails");
  }

  // ---- sequence semantics ---------------------------------------------------
  {
    BehaviourTree tree;
    BTSequence* root = tree.CreateNode<BTSequence>("root");
    root->AddChild(tree.CreateNode<BTCondition>("true", &AlwaysTrue));
    root->AddChild(tree.CreateNode<BTAction>("set_flag", &SetFlag));
    tree.SetRoot(root);

    AIBlackboard blackboard;
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::success, "sequence succeeds when all pass");
    Check(blackboard.GetBoolOrDefault("flag", false), "sequence ran the action");

    BehaviourTree short_circuit;
    BTSequence* failing_root = short_circuit.CreateNode<BTSequence>("root");
    failing_root->AddChild(short_circuit.CreateNode<BTCondition>("false", &AlwaysFalse));
    failing_root->AddChild(short_circuit.CreateNode<BTAction>("set_flag", &SetFlag));
    short_circuit.SetRoot(failing_root);

    AIBlackboard untouched;
    Check(short_circuit.Tick(untouched, 0.016f) == BTNodeStatus::failure, "sequence fails fast");
    Check(!untouched.GetBoolOrDefault("flag", false), "sequence did not run later children");
  }

  // ---- inverter + cooldown --------------------------------------------------
  {
    BehaviourTree tree;
    BTInverter* root = tree.CreateNode<BTInverter>("root");
    root->SetChild(tree.CreateNode<BTCondition>("false", &AlwaysFalse));
    tree.SetRoot(root);

    AIBlackboard blackboard;
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::success, "inverter flips failure to success");
  }
  {
    BehaviourTree tree;
    BTCooldown* root = tree.CreateNode<BTCooldown>("root");
    root->SetCooldownSeconds(1.f);
    root->SetChild(tree.CreateNode<BTAction>("set_flag", &SetFlag));
    tree.SetRoot(root);

    AIBlackboard blackboard;
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::success, "cooldown passes through first tick");
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::failure, "cooldown blocks while cooling down");
  }

  // ---- running action publishes an intent ------------------------------------
  {
    BehaviourTree tree;
    BTSelector* root = tree.CreateNode<BTSelector>("root");
    root->AddChild(tree.CreateNode<BTAction>("move", &MoveSomewhere));
    tree.SetRoot(root);

    AIBlackboard blackboard;
    Check(tree.Tick(blackboard, 0.016f) == BTNodeStatus::running, "running action reports running");
    Check(blackboard.GetMoveIntent().has_move_target, "action published a move intent");
  }

  // ---- perception target selection -------------------------------------------
  {
    AIBlackboard blackboard;
    eastl::vector<AIPerceptionTarget> targets;
    AIPerceptionTarget hidden;
    hidden.object_id = 1;
    hidden.distance = 1.f;
    hidden.is_visible = false;
    targets.push_back(hidden);

    AIPerceptionTarget seen;
    seen.object_id = 2;
    seen.distance = 5.f;
    seen.is_visible = true;
    targets.push_back(seen);

    blackboard.SetPerceivedTargets(eastl::move(targets));
    blackboard.RefreshPrimaryTarget();

    Check(blackboard.HasPrimaryTarget(), "primary target resolved");
    Check(blackboard.GetPrimaryTargetId() == 2, "invisible near target is skipped");
    Check(blackboard.HasLineOfSightToTarget(), "line of sight flag set");
    Check(blackboard.GetDistanceToTarget() == 5.f, "distance cached");
  }

  std::printf("\n%s (failures: %d)\n", (g_failures == 0) ? "AI smoke test PASSED" : "AI smoke test FAILED",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
