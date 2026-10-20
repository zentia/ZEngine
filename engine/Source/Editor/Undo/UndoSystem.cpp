#include "Editor/Undo/UndoSystem.h"

#include <algorithm>
#include <utility>

namespace
{
    const std::string kEmptyTitle;
}  // namespace

// =====================================================================================
// UndoTransaction
// =====================================================================================

bool UndoTransaction::HasRecordWithKey(uint64_t key) const
{
    if (key == 0)
    {
        return false;
    }
    for (const std::unique_ptr<IUndoRecord>& record : m_Records)
    {
        if (record->GetCoalesceKey() == key)
        {
            return true;
        }
    }
    return false;
}

void UndoTransaction::AddRecord(std::unique_ptr<IUndoRecord> record)
{
    if (record == nullptr)
    {
        return;
    }
    // Same object recorded twice inside one transaction: keep the first (oldest
    // "before" state). The second snapshot would only describe an intermediate state.
    if (HasRecordWithKey(record->GetCoalesceKey()))
    {
        return;
    }
    m_Records.push_back(std::move(record));
}

bool UndoTransaction::Finalize()
{
    // Finalize in recording order; a record may legitimately look at the live state
    // other records already produced.
    std::vector<std::unique_ptr<IUndoRecord>> kept;
    kept.reserve(m_Records.size());
    for (std::unique_ptr<IUndoRecord>& record : m_Records)
    {
        if (record->Finalize())
        {
            kept.push_back(std::move(record));
        }
    }
    m_Records = std::move(kept);

    m_MemoryBytes = sizeof(*this) + m_Title.capacity();
    for (const std::unique_ptr<IUndoRecord>& record : m_Records)
    {
        m_MemoryBytes += record->GetMemoryBytes();
    }

    if (m_Context != nullptr)
    {
        m_Context->CaptureAfter();
    }
    return !m_Records.empty();
}

void UndoTransaction::Undo()
{
    for (auto it = m_Records.rbegin(); it != m_Records.rend(); ++it)
    {
        (*it)->Apply();
    }
    if (m_Context != nullptr)
    {
        m_Context->RestoreBefore();
    }
}

void UndoTransaction::Redo()
{
    for (std::unique_ptr<IUndoRecord>& record : m_Records)
    {
        record->Apply();
    }
    if (m_Context != nullptr)
    {
        m_Context->RestoreAfter();
    }
}

// =====================================================================================
// UndoSystem
// =====================================================================================

void UndoSystem::SetLimits(const Limits& limits)
{
    m_Limits = limits;
    if (m_Limits.max_transactions == 0)
    {
        m_Limits.max_transactions = 1;
    }
    EnforceLimits();
}

void UndoSystem::OpenTransaction(const std::string& title, bool implicit)
{
    m_Open = std::make_unique<UndoTransaction>(m_NextTransactionId++, title);
    m_OpenIsImplicit = implicit;
    if (m_ContextFactory)
    {
        std::unique_ptr<IUndoContext> context = m_ContextFactory();
        if (context != nullptr)
        {
            context->CaptureBefore();
            m_Open->SetContext(std::move(context));
        }
    }
}

void UndoSystem::BeginTransaction(const std::string& title)
{
    if (m_Applying)
    {
        // Still count the depth so the matching End balances, but nothing opened
        // here may become an undo step.
        ++m_Depth;
        return;
    }

    if (m_Depth == 0)
    {
        // A pending implicit transaction belongs to a different user action; commit
        // it on its own so this explicit one stays a separate undo step.
        if (m_Open != nullptr && m_OpenIsImplicit)
        {
            CommitOpen();
        }
        if (m_Open == nullptr)
        {
            OpenTransaction(title, /*implicit=*/false);
        }
    }
    ++m_Depth;
}

void UndoSystem::EndTransaction()
{
    if (m_Depth <= 0)
    {
        m_Depth = 0;
        return;
    }
    --m_Depth;
    if (m_Depth == 0 && !m_Applying && m_Open != nullptr && !m_OpenIsImplicit)
    {
        CommitOpen();
    }
}

void UndoSystem::CancelTransaction()
{
    m_Depth = 0;
    if (m_Open == nullptr)
    {
        return;
    }

    // Revert what was recorded so far. The records still hold their "before" state,
    // so one Apply() each (in reverse order) puts the world back.
    std::unique_ptr<UndoTransaction> open = std::move(m_Open);
    m_OpenIsImplicit = false;
    m_Applying = true;
    open->Undo();
    m_Applying = false;
}

bool UndoSystem::AddRecord(std::unique_ptr<IUndoRecord> record, const std::string& implicit_title)
{
    if (record == nullptr || !CanRecord())
    {
        return false;
    }
    if (m_Open == nullptr)
    {
        OpenTransaction(implicit_title, /*implicit=*/true);
    }
    m_Open->AddRecord(std::move(record));
    return true;
}

bool UndoSystem::HasPendingRecord(uint64_t coalesce_key) const
{
    return m_Open != nullptr && m_Open->HasRecordWithKey(coalesce_key);
}

void UndoSystem::FlushImplicitTransaction()
{
    if (m_Depth == 0 && m_Open != nullptr && m_OpenIsImplicit && !m_Applying)
    {
        CommitOpen();
    }
}

void UndoSystem::CommitOpen()
{
    std::unique_ptr<UndoTransaction> transaction = std::move(m_Open);
    m_OpenIsImplicit = false;
    if (transaction == nullptr)
    {
        return;
    }

    if (m_RecordingSuspended || !transaction->Finalize())
    {
        return;  // every record was a no-op (or recording was off): no undo step
    }

    // A new action invalidates the redo branch (linear history, like UE / Unity).
    ClearRedo();

    const std::string title = transaction->GetTitle();
    m_MemoryBytes += transaction->GetMemoryBytes();
    m_UndoStack.push_back(std::move(transaction));
    EnforceLimits();
    Notify(UndoChange::Committed, title);
}

void UndoSystem::EnforceLimits()
{
    // Drop the oldest steps first; always keep at least the most recent one, even if
    // it alone exceeds the memory budget (otherwise a huge delete could not be undone).
    while (m_UndoStack.size() > 1 &&
           (m_UndoStack.size() > m_Limits.max_transactions || m_MemoryBytes > m_Limits.max_memory_bytes))
    {
        m_MemoryBytes -= std::min(m_MemoryBytes, m_UndoStack.front()->GetMemoryBytes());
        m_UndoStack.pop_front();
    }
}

void UndoSystem::ClearRedo()
{
    for (const std::unique_ptr<UndoTransaction>& transaction : m_RedoStack)
    {
        m_MemoryBytes -= std::min(m_MemoryBytes, transaction->GetMemoryBytes());
    }
    m_RedoStack.clear();
}

bool UndoSystem::Undo()
{
    // Flush first so "edit, then immediately Ctrl+Z in the same frame" undoes that
    // edit instead of the one before it.
    FlushImplicitTransaction();
    if (!CanUndo())
    {
        return false;
    }

    std::unique_ptr<UndoTransaction> transaction = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();

    m_Applying = true;
    transaction->Undo();
    m_Applying = false;

    const std::string title = transaction->GetTitle();
    m_RedoStack.push_back(std::move(transaction));
    Notify(UndoChange::Undone, title);
    return true;
}

bool UndoSystem::Redo()
{
    FlushImplicitTransaction();
    if (!CanRedo())
    {
        return false;
    }

    std::unique_ptr<UndoTransaction> transaction = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();

    m_Applying = true;
    transaction->Redo();
    m_Applying = false;

    const std::string title = transaction->GetTitle();
    m_UndoStack.push_back(std::move(transaction));
    Notify(UndoChange::Redone, title);
    return true;
}

size_t UndoSystem::UndoMany(size_t count)
{
    size_t done = 0;
    while (done < count && Undo())
    {
        ++done;
    }
    return done;
}

size_t UndoSystem::RedoMany(size_t count)
{
    size_t done = 0;
    while (done < count && Redo())
    {
        ++done;
    }
    return done;
}

const std::string& UndoSystem::GetUndoTitle() const
{
    return m_UndoStack.empty() ? kEmptyTitle : m_UndoStack.back()->GetTitle();
}

const std::string& UndoSystem::GetRedoTitle() const
{
    return m_RedoStack.empty() ? kEmptyTitle : m_RedoStack.back()->GetTitle();
}

std::vector<std::string> UndoSystem::GetUndoTitles(size_t max_count) const
{
    std::vector<std::string> titles;
    for (auto it = m_UndoStack.rbegin(); it != m_UndoStack.rend() && titles.size() < max_count; ++it)
    {
        titles.push_back((*it)->GetTitle());
    }
    return titles;
}

std::vector<std::string> UndoSystem::GetRedoTitles(size_t max_count) const
{
    std::vector<std::string> titles;
    for (auto it = m_RedoStack.rbegin(); it != m_RedoStack.rend() && titles.size() < max_count; ++it)
    {
        titles.push_back((*it)->GetTitle());
    }
    return titles;
}

uint64_t UndoSystem::GetCurrentStateId() const
{
    return m_UndoStack.empty() ? 0 : m_UndoStack.back()->GetId();
}

void UndoSystem::Clear()
{
    const bool had_anything = !m_UndoStack.empty() || !m_RedoStack.empty() || m_Open != nullptr;
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_Open.reset();
    m_OpenIsImplicit = false;
    m_Depth = 0;
    m_MemoryBytes = 0;
    if (had_anything)
    {
        Notify(UndoChange::Cleared, kEmptyTitle);
    }
}

void UndoSystem::Notify(UndoChange change, const std::string& title)
{
    if (m_Listener)
    {
        m_Listener(change, title);
    }
}
