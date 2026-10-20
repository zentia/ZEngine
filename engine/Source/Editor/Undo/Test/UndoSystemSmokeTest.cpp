// =====================================================================================
// UndoSystemSmokeTest -- headless checks for the engine-agnostic undo stack.
// -------------------------------------------------------------------------------------
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_UNDO_SMOKE_TEST=ON
//   cmake --build build --target UndoSystemSmokeTest
//
// The test links only UndoSystem.cpp: no engine, no editor, no device. Records here
// edit plain ints so every expectation is exact.
// =====================================================================================

#include "Editor/Undo/UndoSystem.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    int g_Failures = 0;
    int g_Checks = 0;

    void Check(bool condition, const char* what, int line)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("  FAIL (line %d): %s\n", line, what);
        }
    }

#define CHECK(cond) Check((cond), #cond, __LINE__)

    // Swap-based record over one int, the same contract EditorUndo's records follow.
    class IntRecord : public IUndoRecord
    {
    public:
        IntRecord(int* target, uint64_t key = 0)
            : m_Target(target), m_Stored(*target), m_Key(key)
        {
        }

        void Apply() override
        {
            const int live = *m_Target;
            *m_Target = m_Stored;
            m_Stored = live;
        }

        bool Finalize() override { return *m_Target != m_Stored; }
        uint64_t GetCoalesceKey() const override { return m_Key; }
        size_t GetMemoryBytes() const override { return m_Bytes; }

        size_t m_Bytes {sizeof(IntRecord)};

    private:
        int* m_Target;
        int m_Stored;
        uint64_t m_Key;
    };

    // Records the order Apply() runs in, to prove undo = reverse, redo = forward.
    class OrderRecord : public IUndoRecord
    {
    public:
        OrderRecord(std::vector<int>* log, int tag)
            : m_Log(log), m_Tag(tag)
        {
        }
        void Apply() override { m_Log->push_back(m_Tag); }

    private:
        std::vector<int>* m_Log;
        int m_Tag;
    };

    struct SelectionWorld
    {
        int selected = 0;
    };

    class SelectionContext : public IUndoContext
    {
    public:
        explicit SelectionContext(SelectionWorld* world)
            : m_World(world)
        {
        }
        void CaptureBefore() override { m_Before = m_World->selected; }
        void CaptureAfter() override { m_After = m_World->selected; }
        void RestoreBefore() override { m_World->selected = m_Before; }
        void RestoreAfter() override { m_World->selected = m_After; }

    private:
        SelectionWorld* m_World;
        int m_Before = 0;
        int m_After = 0;
    };

    void Edit(UndoSystem& undo, int* value, int new_value, uint64_t key = 0)
    {
        undo.AddRecord(std::make_unique<IntRecord>(value, key));
        *value = new_value;
    }

    void TestBasicUndoRedo()
    {
        std::printf("[1] basic undo / redo\n");
        UndoSystem undo;
        int a = 1;

        undo.BeginTransaction("Set A");
        Edit(undo, &a, 2);
        undo.EndTransaction();

        CHECK(undo.GetUndoCount() == 1);
        CHECK(undo.GetUndoTitle() == "Set A");
        CHECK(undo.Undo());
        CHECK(a == 1);
        CHECK(undo.GetRedoTitle() == "Set A");
        CHECK(undo.Redo());
        CHECK(a == 2);
        CHECK(!undo.Redo());
        CHECK(undo.Undo());
        CHECK(!undo.Undo());
        CHECK(a == 1);
    }

    void TestNestingFoldsIntoOne()
    {
        std::printf("[2] nested transactions fold into the outermost\n");
        UndoSystem undo;
        int a = 0;
        int b = 0;

        undo.BeginTransaction("Outer");
        Edit(undo, &a, 1);
        undo.BeginTransaction("Inner");
        Edit(undo, &b, 1);
        undo.EndTransaction();
        CHECK(undo.GetUndoCount() == 0);  // still open
        undo.EndTransaction();

        CHECK(undo.GetUndoCount() == 1);
        CHECK(undo.GetUndoTitle() == "Outer");
        undo.Undo();
        CHECK(a == 0 && b == 0);
    }

    void TestOrder()
    {
        std::printf("[3] undo runs records in reverse, redo forward\n");
        UndoSystem undo;
        std::vector<int> log;
        undo.BeginTransaction("Ordered");
        undo.AddRecord(std::make_unique<OrderRecord>(&log, 1));
        undo.AddRecord(std::make_unique<OrderRecord>(&log, 2));
        undo.AddRecord(std::make_unique<OrderRecord>(&log, 3));
        undo.EndTransaction();

        undo.Undo();
        CHECK((log == std::vector<int> {3, 2, 1}));
        log.clear();
        undo.Redo();
        CHECK((log == std::vector<int> {1, 2, 3}));
    }

    void TestNoOpIsDropped()
    {
        std::printf("[4] a transaction whose records changed nothing is discarded\n");
        UndoSystem undo;
        int a = 5;
        undo.BeginTransaction("Commit same value");
        Edit(undo, &a, 5);
        undo.EndTransaction();
        CHECK(undo.GetUndoCount() == 0);

        // Mixed: one real change survives, the no-op record is dropped.
        int b = 1;
        undo.BeginTransaction("Mixed");
        Edit(undo, &a, 5);
        Edit(undo, &b, 9);
        undo.EndTransaction();
        CHECK(undo.GetUndoCount() == 1);
        undo.Undo();
        CHECK(b == 1 && a == 5);
    }

    void TestCoalescing()
    {
        std::printf("[5] same key twice in one transaction keeps the oldest state\n");
        UndoSystem undo;
        int a = 0;
        undo.BeginTransaction("Drag");
        for (int step = 1; step <= 10; ++step)
        {
            Edit(undo, &a, step, /*key=*/42);
        }
        CHECK(undo.HasPendingRecord(42));
        undo.EndTransaction();
        CHECK(a == 10);
        undo.Undo();
        CHECK(a == 0);
        undo.Redo();
        CHECK(a == 10);
    }

    void TestRedoBranchClearedByNewEdit()
    {
        std::printf("[6] a new edit after undo clears the redo branch\n");
        UndoSystem undo;
        int a = 0;
        undo.BeginTransaction("1");
        Edit(undo, &a, 1);
        undo.EndTransaction();
        undo.BeginTransaction("2");
        Edit(undo, &a, 2);
        undo.EndTransaction();

        undo.Undo();
        CHECK(undo.GetRedoCount() == 1);
        undo.BeginTransaction("3");
        Edit(undo, &a, 3);
        undo.EndTransaction();
        CHECK(undo.GetRedoCount() == 0);
        CHECK(!undo.Redo());
        undo.Undo();
        CHECK(a == 1);
    }

    void TestImplicitTransaction()
    {
        std::printf("[7] records without Begin group until the frame flush\n");
        UndoSystem undo;
        int a = 0;
        int b = 0;
        Edit(undo, &a, 1);
        Edit(undo, &b, 1);
        CHECK(undo.IsTransactionOpen());
        CHECK(!undo.IsExplicitTransactionOpen());
        undo.FlushImplicitTransaction();
        CHECK(undo.GetUndoCount() == 1);
        undo.Undo();
        CHECK(a == 0 && b == 0);

        // An explicit Begin closes a pending implicit one as its own step.
        undo.Redo();
        int c = 0;
        Edit(undo, &c, 7);
        undo.BeginTransaction("Explicit");
        Edit(undo, &a, 5);
        undo.EndTransaction();
        CHECK(undo.GetUndoCount() == 3);
        CHECK(undo.GetUndoTitle() == "Explicit");

        // Undo() itself flushes a pending implicit step first.
        int d = 0;
        Edit(undo, &d, 3);
        CHECK(undo.Undo());
        CHECK(d == 0);
        CHECK(a == 5);
    }

    void TestUndoRefusedWhileOpen()
    {
        std::printf("[8] undo is refused while an explicit transaction is open\n");
        UndoSystem undo;
        int a = 0;
        undo.BeginTransaction("Earlier");
        Edit(undo, &a, 1);
        undo.EndTransaction();

        undo.BeginTransaction("Gizmo drag");
        Edit(undo, &a, 2);
        CHECK(!undo.CanUndo());
        CHECK(!undo.Undo());
        CHECK(a == 2);
        undo.EndTransaction();
        CHECK(undo.Undo());
        CHECK(a == 1);
    }

    void TestCancel()
    {
        std::printf("[9] cancel reverts the open transaction and records nothing\n");
        UndoSystem undo;
        int a = 0;
        undo.BeginTransaction("Cancelled");
        Edit(undo, &a, 1, 1);
        Edit(undo, &a, 2, 1);
        undo.CancelTransaction();
        CHECK(a == 0);
        CHECK(undo.GetUndoCount() == 0);
        CHECK(!undo.IsTransactionOpen());
        undo.EndTransaction();  // stray End after cancel must be harmless
        CHECK(undo.GetTransactionDepth() == 0);
    }

    void TestRecordingGuards()
    {
        std::printf("[10] no recording while applying or while suspended\n");
        UndoSystem undo;
        int a = 0;

        // A record that tries to record during Apply().
        class ReentrantRecord : public IUndoRecord
        {
        public:
            ReentrantRecord(UndoSystem* undo, int* value)
                : m_Undo(undo), m_Value(value)
            {
            }
            void Apply() override
            {
                accepted = m_Undo->AddRecord(std::make_unique<IntRecord>(m_Value));
                ++applied;
            }
            UndoSystem* m_Undo;
            int* m_Value;
            bool accepted = true;
            int applied = 0;
        };

        auto record = std::make_unique<ReentrantRecord>(&undo, &a);
        ReentrantRecord* raw = record.get();
        undo.BeginTransaction("Reentrant");
        undo.AddRecord(std::move(record));
        undo.EndTransaction();
        undo.Undo();
        CHECK(raw->applied == 1);
        CHECK(!raw->accepted);
        CHECK(undo.GetRedoCount() == 1);
        CHECK(!undo.IsTransactionOpen());

        UndoSystem suspended;
        suspended.SetRecordingSuspended(true);
        suspended.BeginTransaction("Play mode");
        CHECK(!suspended.AddRecord(std::make_unique<IntRecord>(&a)));
        suspended.EndTransaction();
        CHECK(suspended.GetUndoCount() == 0);
    }

    void TestLimits()
    {
        std::printf("[11] count and memory limits trim the oldest steps\n");
        UndoSystem undo;
        UndoSystem::Limits limits;
        limits.max_transactions = 3;
        undo.SetLimits(limits);

        int a = 0;
        for (int step = 1; step <= 5; ++step)
        {
            undo.BeginTransaction(std::to_string(step));
            Edit(undo, &a, step);
            undo.EndTransaction();
        }
        CHECK(undo.GetUndoCount() == 3);
        CHECK(undo.UndoMany(10) == 3);
        CHECK(a == 2);  // steps 1 and 2 were trimmed

        UndoSystem budget;
        int b = 0;
        auto commit_big_step = [&budget, &b](int value) {
            auto record = std::make_unique<IntRecord>(&b);
            record->m_Bytes = 400;
            budget.BeginTransaction("big");
            budget.AddRecord(std::move(record));
            b = value;
            budget.EndTransaction();
        };
        // Measure one step (record + transaction bookkeeping), then allow 2.5 steps:
        // two fit, three do not.
        commit_big_step(1);
        const size_t per_step = budget.GetMemoryBytes();
        CHECK(per_step >= 400);
        UndoSystem::Limits memory_limits;
        memory_limits.max_memory_bytes = per_step * 2 + per_step / 2;
        budget.SetLimits(memory_limits);
        for (int step = 2; step <= 4; ++step)
        {
            commit_big_step(step);
        }
        CHECK(budget.GetUndoCount() == 2);
        CHECK(budget.GetMemoryBytes() <= memory_limits.max_memory_bytes);

        // The newest step is always kept, even if it alone is over budget.
        UndoSystem tiny;
        UndoSystem::Limits tiny_limits;
        tiny_limits.max_memory_bytes = 10;
        tiny.SetLimits(tiny_limits);
        auto record = std::make_unique<IntRecord>(&b);
        record->m_Bytes = 4000;
        tiny.BeginTransaction("huge");
        tiny.AddRecord(std::move(record));
        b = 100;
        tiny.EndTransaction();
        CHECK(tiny.GetUndoCount() == 1);
    }

    void TestContext()
    {
        std::printf("[12] transaction context restores selection around records\n");
        UndoSystem undo;
        SelectionWorld world;
        world.selected = 1;
        undo.SetContextFactory([&world]() { return std::make_unique<SelectionContext>(&world); });

        int a = 0;
        undo.BeginTransaction("Create");
        Edit(undo, &a, 1);
        world.selected = 2;  // the created object gets selected
        undo.EndTransaction();

        world.selected = 3;  // user clicks elsewhere afterwards
        undo.Undo();
        CHECK(world.selected == 1);
        undo.Redo();
        CHECK(world.selected == 2);
    }

    void TestListenerAndStateId()
    {
        std::printf("[13] change listener and save-point state id\n");
        UndoSystem undo;
        std::vector<UndoChange> changes;
        undo.SetChangeListener([&changes](UndoChange change, const std::string&) { changes.push_back(change); });

        CHECK(undo.GetCurrentStateId() == 0);
        int a = 0;
        undo.BeginTransaction("1");
        Edit(undo, &a, 1);
        undo.EndTransaction();
        const uint64_t saved = undo.GetCurrentStateId();
        CHECK(saved != 0);

        undo.BeginTransaction("2");
        Edit(undo, &a, 2);
        undo.EndTransaction();
        CHECK(undo.GetCurrentStateId() != saved);
        undo.Undo();
        CHECK(undo.GetCurrentStateId() == saved);
        undo.Undo();
        CHECK(undo.GetCurrentStateId() == 0);
        undo.Clear();
        CHECK(undo.GetUndoCount() == 0 && undo.GetRedoCount() == 0);

        const std::vector<UndoChange> expected = {UndoChange::Committed, UndoChange::Committed, UndoChange::Undone,
                                                  UndoChange::Undone, UndoChange::Cleared};
        CHECK(changes == expected);
    }

    void TestTitles()
    {
        std::printf("[14] history titles, most recent first\n");
        UndoSystem undo;
        int a = 0;
        for (int step = 1; step <= 4; ++step)
        {
            undo.BeginTransaction("Step " + std::to_string(step));
            Edit(undo, &a, step);
            undo.EndTransaction();
        }
        undo.Undo();
        const std::vector<std::string> undo_titles = undo.GetUndoTitles(2);
        CHECK((undo_titles == std::vector<std::string> {"Step 3", "Step 2"}));
        const std::vector<std::string> redo_titles = undo.GetRedoTitles(5);
        CHECK((redo_titles == std::vector<std::string> {"Step 4"}));
    }
}  // namespace

int main()
{
    std::printf("UndoSystemSmokeTest\n");
    TestBasicUndoRedo();
    TestNestingFoldsIntoOne();
    TestOrder();
    TestNoOpIsDropped();
    TestCoalescing();
    TestRedoBranchClearedByNewEdit();
    TestImplicitTransaction();
    TestUndoRefusedWhileOpen();
    TestCancel();
    TestRecordingGuards();
    TestLimits();
    TestContext();
    TestListenerAndStateId();
    TestTitles();

    std::printf("%d checks, %d failures\n", g_Checks, g_Failures);
    std::printf(g_Failures == 0 ? "UndoSystemSmokeTest: PASS\n" : "UndoSystemSmokeTest: FAIL\n");
    return g_Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
