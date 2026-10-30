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
}  // namespace

Behaviour::~Behaviour()
{
    CancelAllInvokes();
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
