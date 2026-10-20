#pragma once

// =====================================================================================
// UndoSystem -- engine-agnostic transaction stack (UE FTransBuffer + Unity Undo hybrid).
// -------------------------------------------------------------------------------------
// This header deliberately depends on nothing but the standard library so the whole
// stack logic can be unit tested headless (see Editor/Undo/Test/UndoSystemSmokeTest.cpp).
// Everything that knows about GameObjects, Components, selection or the level lives in
// EditorUndo.{h,cpp} / EditorUndoRecords.{h,cpp} on top of this.
//
// Model
//   * A transaction is a titled, ordered list of records. Undo applies the records in
//     reverse order, redo in forward order. One Ctrl+Z == one transaction.
//   * A record is SWAP-based (the UE FTransaction "flip" model): Apply() exchanges the
//     state it stores with the live state. The same call therefore serves both undo
//     and redo, and the record never has to be told which direction it is going. The
//     record captures the "before" state when it is created; Finalize() runs once when
//     the transaction closes and may report the record as a no-op (state unchanged),
//     in which case it is dropped.
//   * Transactions nest: only the outermost Begin/End pair creates an undo step, inner
//     pairs are folded into it (UE FScopedTransaction semantics).
//   * Recording without an open transaction is allowed (Unity Undo.RecordObject
//     semantics): the records go into an IMPLICIT transaction that stays open until
//     FlushImplicitTransaction(), which the editor calls once per frame. Everything one
//     event handler does therefore lands in one undo step even if the caller never
//     opened a transaction.
//   * An optional per-transaction context (IUndoContext) carries side state that is not
//     a record of its own -- the editor uses it for the selection, which UE also
//     restores as part of every transaction.
// =====================================================================================

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// One reversible change. Implementations store ONE state and swap it with the live
/// state on every Apply(); see the header comment above.
class IUndoRecord
{
public:
    virtual ~IUndoRecord() = default;

    /// Exchange the stored state with the live state. Must be its own inverse:
    /// Apply(); Apply(); leaves the world exactly as before.
    virtual void Apply() = 0;

    /// Called once when the owning transaction closes, before it is pushed onto the
    /// undo stack. Return false if the record turned out to be a no-op (nothing
    /// changed); it is then dropped, and a transaction whose every record is dropped
    /// is discarded entirely, so "commit the same value" never pollutes the history.
    virtual bool Finalize() { return true; }

    /// Records with the same non-zero key inside ONE transaction collapse to the
    /// first one (the oldest "before" state wins, matching Unity's RecordObject being
    /// called several times on the same object within one undo group).
    virtual uint64_t GetCoalesceKey() const { return 0; }

    /// Approximate heap footprint, used for the history memory budget.
    virtual size_t GetMemoryBytes() const { return sizeof(*this); }
};

/// Transaction-level side state captured around the records (editor: selection).
/// Created when the outermost transaction opens; CaptureBefore() runs at that moment,
/// CaptureAfter() when the transaction closes.
class IUndoContext
{
public:
    virtual ~IUndoContext() = default;
    virtual void CaptureBefore() = 0;
    virtual void CaptureAfter() = 0;
    virtual void RestoreBefore() = 0;  // after an undo
    virtual void RestoreAfter() = 0;   // after a redo
};

class UndoTransaction
{
public:
    UndoTransaction(uint64_t id, std::string title)
        : m_Id(id), m_Title(std::move(title))
    {
    }

    uint64_t GetId() const { return m_Id; }
    const std::string& GetTitle() const { return m_Title; }
    void SetTitle(std::string title) { m_Title = std::move(title); }
    size_t GetRecordCount() const { return m_Records.size(); }
    size_t GetMemoryBytes() const { return m_MemoryBytes; }

    bool HasRecordWithKey(uint64_t key) const;
    void AddRecord(std::unique_ptr<IUndoRecord> record);
    void SetContext(std::unique_ptr<IUndoContext> context) { m_Context = std::move(context); }

    /// Run Finalize() on every record and drop the no-ops. Returns false when nothing
    /// is left (the transaction should then be discarded).
    bool Finalize();

    void Undo();
    void Redo();

private:
    uint64_t m_Id {0};
    std::string m_Title;
    std::vector<std::unique_ptr<IUndoRecord>> m_Records;
    std::unique_ptr<IUndoContext> m_Context;
    size_t m_MemoryBytes {0};
};

enum class UndoChange
{
    Committed,  // a transaction was pushed onto the undo stack
    Undone,
    Redone,
    Cleared,
};

class UndoSystem
{
public:
    using ContextFactory = std::function<std::unique_ptr<IUndoContext>()>;
    using ChangeListener = std::function<void(UndoChange change, const std::string& title)>;

    struct Limits
    {
        size_t max_transactions = 200;                 // UE default is 100, Unity is unbounded
        size_t max_memory_bytes = 256u * 1024u * 1024u;
    };

    UndoSystem() = default;
    UndoSystem(const UndoSystem&) = delete;
    UndoSystem& operator=(const UndoSystem&) = delete;

    // ---- configuration --------------------------------------------------------------
    void SetContextFactory(ContextFactory factory) { m_ContextFactory = std::move(factory); }
    void SetChangeListener(ChangeListener listener) { m_Listener = std::move(listener); }
    void SetLimits(const Limits& limits);
    const Limits& GetLimits() const { return m_Limits; }

    /// While suspended, Begin/End still balance but every record is discarded (the
    /// editor suspends recording in play mode: play-world edits are not undoable).
    void SetRecordingSuspended(bool suspended) { m_RecordingSuspended = suspended; }
    bool IsRecordingSuspended() const { return m_RecordingSuspended; }

    /// True while an undo / redo is being applied. Records offered during that window
    /// are ignored -- applying a record must never record another one.
    bool IsApplying() const { return m_Applying; }

    /// True when a record offered right now would actually be kept.
    bool CanRecord() const { return !m_Applying && !m_RecordingSuspended; }

    // ---- recording ------------------------------------------------------------------
    /// Open a transaction. Nested calls fold into the outermost one. If an implicit
    /// transaction is pending it is closed first, so it stays a separate undo step.
    void BeginTransaction(const std::string& title);

    /// Close the innermost Begin. The outermost End finalizes and commits.
    void EndTransaction();

    /// Revert everything recorded so far in the open transaction and discard it
    /// (UE FScopedTransaction::Cancel). Closes ALL nesting levels.
    void CancelTransaction();

    /// Depth of explicit Begin calls currently open.
    int GetTransactionDepth() const { return m_Depth; }
    bool IsTransactionOpen() const { return m_Open != nullptr; }
    bool IsExplicitTransactionOpen() const { return m_Depth > 0; }

    /// Add a record to the open transaction, or to the implicit one if none is open.
    /// `implicit_title` names the implicit transaction when this record opens it.
    /// Returns false (and destroys the record) when recording is not possible.
    bool AddRecord(std::unique_ptr<IUndoRecord> record, const std::string& implicit_title = "Edit");

    /// True if the open transaction already holds a record with this coalesce key.
    /// Callers use it to skip the (possibly expensive) snapshot entirely.
    bool HasPendingRecord(uint64_t coalesce_key) const;

    /// Commit the implicit transaction, if one is pending and no explicit one is open.
    void FlushImplicitTransaction();

    // ---- undo / redo ------------------------------------------------------------------
    bool CanUndo() const { return !m_UndoStack.empty() && m_Open == nullptr && !m_Applying; }
    bool CanRedo() const { return !m_RedoStack.empty() && m_Open == nullptr && !m_Applying; }

    /// Refuses (returns false) while a transaction is open, e.g. mid gizmo drag.
    bool Undo();
    bool Redo();

    /// Undo / redo several steps; returns how many actually ran.
    size_t UndoMany(size_t count);
    size_t RedoMany(size_t count);

    const std::string& GetUndoTitle() const;
    const std::string& GetRedoTitle() const;

    size_t GetUndoCount() const { return m_UndoStack.size(); }
    size_t GetRedoCount() const { return m_RedoStack.size(); }

    /// Titles, most recent first.
    std::vector<std::string> GetUndoTitles(size_t max_count) const;
    std::vector<std::string> GetRedoTitles(size_t max_count) const;

    /// Identity of the state the world is in: the id of the transaction on top of the
    /// undo stack (0 = nothing done since the history started). Used to track the
    /// save point -- undoing back to the saved state clears the scene's dirty flag.
    uint64_t GetCurrentStateId() const;

    size_t GetMemoryBytes() const { return m_MemoryBytes; }

    /// Drop all history (and any open transaction, without reverting it).
    void Clear();

private:
    void OpenTransaction(const std::string& title, bool implicit);
    void CommitOpen();
    void EnforceLimits();
    void ClearRedo();
    void Notify(UndoChange change, const std::string& title);

    std::deque<std::unique_ptr<UndoTransaction>> m_UndoStack;  // back() = most recent
    std::deque<std::unique_ptr<UndoTransaction>> m_RedoStack;  // back() = next redo
    std::unique_ptr<UndoTransaction> m_Open;

    ContextFactory m_ContextFactory;
    ChangeListener m_Listener;
    Limits m_Limits;

    uint64_t m_NextTransactionId {1};
    size_t m_MemoryBytes {0};
    int m_Depth {0};
    bool m_OpenIsImplicit {false};
    bool m_Applying {false};
    bool m_RecordingSuspended {false};
};
