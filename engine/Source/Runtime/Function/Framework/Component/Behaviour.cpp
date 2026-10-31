#include "Runtime/Function/Framework/Component/Behaviour.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Framework/Coroutine/Coroutine.h"
#include "Runtime/BaseClasses/GameObject.h"

namespace
{
    /// One delayed call.
    ///
    /// `self` is a raw pointer on purpose: the routine is owned by the
    /// scheduler, and Behaviour's destructor cancels it. Holding a PPtr here
    /// would keep the component alive for one more frame after a cancel, which
    /// is not what Unity does.
    Coroutine InvokeOnceRoutine(Behaviour* self, eastl::string method, float delay)
    {
        co_yield WaitForSeconds {delay};
        self->FireInvoke(method);
    }

    /// The same, forever. Only CancelInvoke (or the destructor) ends it.
    Coroutine InvokeRepeatingRoutine(Behaviour* self, eastl::string method, float delay, float repeat_rate)
    {
        co_yield WaitForSeconds {delay};
        for (;;)
        {
            self->FireInvoke(method);
            co_yield WaitForSeconds {repeat_rate};
        }
    }
    /// One script generator, pumped.
    ///
    /// The body lives on the script side; this coroutine is only the adapter
    /// between "the scheduler resumed me" and "call `next()` once". Each resume
    /// therefore costs exactly one `generator.next()` -- a script coroutine can
    /// never run ahead of the frame loop, even if its body yields in a loop.
    ///
    /// `self` is a raw pointer for the same reason InvokeOnceRoutine's is: the
    /// coroutine is owned by the scheduler and cancelled from Behaviour's
    /// destructor, so a PPtr here would extend the component's lifetime by one
    /// frame past its own destruction.
    Coroutine ScriptCoroutinePump(Behaviour* self, uint64_t script_id)
    {
        for (;;)
        {
            ScriptYield yielded;
            if (!self->StepScriptCoroutine(script_id, yielded))
            {
                // 生成器不再应答：释放它然后退出。不放的话这个 JS 值会一直
                // 挂到下一次 Start / IsCoroutineRunning 触发剪枝为止 —— 而
                // 那两个调用可能永远不来。
                self->ReleaseScriptCoroutine(script_id);
                co_return;
            }
            if (yielded.done)
            {
                self->ReleaseScriptCoroutine(script_id);
                co_return;
            }

            switch (yielded.kind)
            {
                case ScriptYieldKind::kSeconds:
                    co_yield WaitForSeconds {yielded.seconds};
                    break;
                case ScriptYieldKind::kSecondsRealtime:
                    co_yield WaitForSecondsRealtime {yielded.seconds};
                    break;
                case ScriptYieldKind::kFixedUpdate:
                    co_yield WaitForFixedUpdate {};
                    break;
                case ScriptYieldKind::kLateUpdate:
                    co_yield WaitForLateUpdate {};
                    break;
                case ScriptYieldKind::kEndOfFrame:
                    co_yield WaitForEndOfFrame {};
                    break;
                case ScriptYieldKind::kNull:
                default:
                    // 未知标记在 ScriptEnv 里已经降级成 kNull 并记了日志，这里
                    // 不再吵一次。
                    co_yield WaitForNextFrame {};
                    break;
            }
        }
    }
}  // namespace

Behaviour::~Behaviour()
{
    CancelAllInvokes();
    StopAllCoroutines();
}

void Behaviour::Invoke(const eastl::string& methodName, float time)
{
    if (methodName.empty() || time <= 0.f)
    {
        LOG_WARNING(ZObject, "Behaviour.Invoke: needs a method name and a positive delay");
        return;
    }

    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler == nullptr)
    {
        LOG_WARNING(ZObject, "Behaviour.Invoke: no coroutine scheduler (engine not running?)");
        return;
    }

    const uint64_t id = scheduler->Start(InvokeOnceRoutine(this, methodName, time),
                                         reinterpret_cast<uint64_t>(this));
    if (id == CoroutineScheduler::kInvalidId)
    {
        return;
    }

    m_Invokes.push_back({methodName, id});
}

void Behaviour::InvokeRepeating(const eastl::string& methodName, float time, float repeatRate)
{
    if (methodName.empty() || time <= 0.f || repeatRate <= 0.f)
    {
        LOG_WARNING(ZObject,
                    "Behaviour.InvokeRepeating: needs a method name, a positive delay and a positive "
                    "repeat rate");
        return;
    }

    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler == nullptr)
    {
        LOG_WARNING(ZObject, "Behaviour.InvokeRepeating: no coroutine scheduler (engine not running?)");
        return;
    }

    const uint64_t id = scheduler->Start(
        InvokeRepeatingRoutine(this, methodName, time, repeatRate),
        reinterpret_cast<uint64_t>(this));
    if (id == CoroutineScheduler::kInvalidId)
    {
        return;
    }

    m_Invokes.push_back({methodName, id});
}

void Behaviour::CancelInvoke(const eastl::string& methodName)
{
    // Unity: CancelInvoke() with no argument cancels every invoke on the
    // behaviour. An empty string is the only spelling pesapi can carry, so it
    // doubles as "no argument".
    if (methodName.empty())
    {
        CancelAllInvokes();
        return;
    }

    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);

    eastl::vector<PendingInvoke> kept;
    kept.reserve(m_Invokes.size());
    for (PendingInvoke& pending : m_Invokes)
    {
        if (pending.method == methodName)
        {
            if (scheduler != nullptr)
            {
                scheduler->Stop(pending.id);
            }
            continue;
        }
        kept.push_back(pending);
    }
    m_Invokes.swap(kept);
}

void Behaviour::CancelAllInvokes()
{
    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler != nullptr)
    {
        for (const PendingInvoke& pending : m_Invokes)
        {
            scheduler->Stop(pending.id);
        }
    }
    m_Invokes.clear();
}

bool Behaviour::IsInvoking(const eastl::string& methodName)
{
    PruneFinishedInvokes();

    for (const PendingInvoke& pending : m_Invokes)
    {
        if (pending.method == methodName)
        {
            return true;
        }
    }
    return false;
}

bool Behaviour::InvokeBehaviourMethod([[maybe_unused]] const char* method_name)
{
    return false;
}

void Behaviour::FireInvoke(const eastl::string& method_name)
{
    if (InvokeBehaviourMethod(method_name.c_str()))
    {
        return;
    }

    if (m_ParentObject != nullptr)
    {
        m_ParentObject->SendMessageByName(method_name);
    }
}

uint64_t Behaviour::StartCoroutine(const eastl::string& methodName)
{
    if (methodName.empty())
    {
        LOG_WARNING(ZObject, "Behaviour.StartCoroutine: needs a method name");
        return CoroutineScheduler::kInvalidId;
    }

    PruneFinishedCoroutines();

    uint64_t script_id = 0;
    if (!StartScriptCoroutine(methodName.c_str(), script_id))
    {
        LOG_WARNING(ZObject,
                    "Behaviour.StartCoroutine('{}'): this behaviour has no script side, "
                    "or the method did not return a generator",
                    methodName.c_str());
        return CoroutineScheduler::kInvalidId;
    }

    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler == nullptr)
    {
        LOG_WARNING(ZObject, "Behaviour.StartCoroutine: no coroutine scheduler (engine not running?)");
        EndScriptCoroutine(script_id);
        return CoroutineScheduler::kInvalidId;
    }

    const uint64_t id = scheduler->Start(ScriptCoroutinePump(this, script_id),
                                         reinterpret_cast<uint64_t>(this));
    if (id == CoroutineScheduler::kInvalidId)
    {
        // 已经跑完（第一次 resume 就 done）—— 生成器还是要放掉。
        EndScriptCoroutine(script_id);
        return CoroutineScheduler::kInvalidId;
    }

    m_Coroutines.push_back({methodName, id, script_id});
    return id;
}

void Behaviour::StopCoroutine(const eastl::string& methodName)
{
    if (methodName.empty())
    {
        return;
    }

    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);

    eastl::vector<PendingCoroutine> kept;
    kept.reserve(m_Coroutines.size());
    for (PendingCoroutine& pending : m_Coroutines)
    {
        if (pending.method != methodName)
        {
            kept.push_back(pending);
            continue;
        }
        if (scheduler != nullptr)
        {
            scheduler->Stop(pending.id);
        }
        ReleasePendingGenerator(pending);
    }
    m_Coroutines.swap(kept);
}

void Behaviour::StopAllCoroutines()
{
    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler != nullptr)
    {
        for (const PendingCoroutine& pending : m_Coroutines)
        {
            scheduler->Stop(pending.id);
        }
    }
    for (PendingCoroutine& pending : m_Coroutines)
    {
        ReleasePendingGenerator(pending);
    }
    m_Coroutines.clear();
}

bool Behaviour::IsCoroutineRunning(const eastl::string& methodName)
{
    PruneFinishedCoroutines();

    for (const PendingCoroutine& pending : m_Coroutines)
    {
        if (pending.method == methodName)
        {
            return true;
        }
    }
    return false;
}

size_t Behaviour::GetCoroutineCount() const
{
    // 不是 const 方法里的 Prune：剪枝要调 EndScriptCoroutine（会释放 JS 引用），
    // 属于状态变更。这里只报"当前登记的条数"，调用方想拿到准确值先调
    // IsCoroutineRunning 或直接看 scheduler。
    return m_Coroutines.size();
}

bool Behaviour::StartScriptCoroutine([[maybe_unused]] const char* method_name,
                                     [[maybe_unused]] uint64_t& out_script_id)
{
    return false;
}

bool Behaviour::StepScriptCoroutine([[maybe_unused]] uint64_t script_id,
                                    [[maybe_unused]] ScriptYield& out)
{
    return false;
}

void Behaviour::EndScriptCoroutine([[maybe_unused]] uint64_t script_id) {}

void Behaviour::ReleaseScriptCoroutine(uint64_t script_id)
{
    for (PendingCoroutine& pending : m_Coroutines)
    {
        if (pending.script_id == script_id)
        {
            ReleasePendingGenerator(pending);
            return;
        }
    }
    // Not registered yet: the body ran out during `Start()`'s synchronous
    // first resume. StartCoroutine's kInvalidId branch releases it there.
}

void Behaviour::ReleasePendingGenerator(PendingCoroutine& entry)
{
    if (!entry.generator_owned)
    {
        return;
    }
    entry.generator_owned = false;
    EndScriptCoroutine(entry.script_id);
}

void Behaviour::PruneFinishedCoroutines()
{
    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler == nullptr)
    {
        return;
    }

    eastl::vector<PendingCoroutine> kept;
    kept.reserve(m_Coroutines.size());
    for (PendingCoroutine& pending : m_Coroutines)
    {
        if (scheduler->IsRunning(pending.id))
        {
            kept.push_back(pending);
            continue;
        }
        ReleasePendingGenerator(pending);
    }
    m_Coroutines.swap(kept);
}

void Behaviour::PruneFinishedInvokes()
{
    CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
    if (scheduler == nullptr)
    {
        return;
    }

    eastl::vector<PendingInvoke> kept;
    kept.reserve(m_Invokes.size());
    for (PendingInvoke& pending : m_Invokes)
    {
        if (scheduler->IsRunning(pending.id))
        {
            kept.push_back(pending);
        }
    }
    m_Invokes.swap(kept);
}
