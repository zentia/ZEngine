#pragma once

#include "Runtime/Function/Framework/Component/Transform/TransformAccess.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeSystemMask.h"
#include "Runtime/Function/Framework/Component/Transform/TransformHierarchyTypes.h"

#include <functional>
#include <string>
#include <vector>

class Transform;

struct TransformChange
{
    TransformAccessReadOnly access;
    TransformChangeSystemMask mask {0};
};

using TransformChangeBatchCallback = std::function<void(const TransformChange* changes, size_t count)>;

class TransformChangeDispatch
{
public:
    enum InterestType : uint64_t
    {
        kInterestedInGlobalT = 1ull << 0,
        kInterestedInGlobalR = 1ull << 1,
        kInterestedInGlobalS = 1ull << 2,
        kInterestedInRendererUpdate = 1ull << 3,
        kInterestedInParentHierarchy = 1ull << 4,
        kInterestedInSiblingOrder = 1ull << 5,
        kInterestedInLocalT = 1ull << 7,
        kInterestedInLocalR = 1ull << 8,
        kInterestedInLocalS = 1ull << 9,
        kInterestedInLocalTRS = kInterestedInLocalT | kInterestedInLocalR | kInterestedInLocalS,
        kInterestedInGlobalTRS = kInterestedInGlobalT | kInterestedInGlobalR | kInterestedInGlobalS,
    };

    TransformChangeSystemHandle RegisterSystem(const char* name, InterestType interest_type);
    void UnregisterSystem(TransformChangeSystemHandle& system);

    TransformChangeSystemMask GetInterestMask(InterestType interest_type) const;

    void SetSystemInterested(TransformAccessReadOnly access, TransformChangeSystemHandle system, bool interested);
    void SetSystemInterested(TransformAccessReadOnly access, InterestType interest_type, bool interested);

    void QueueTransformChangeIfHasChanged(TransformAccess access);

    void NotifyParentHierarchyChanged(TransformAccessReadOnly access);
    TransformChangeSystemMask GetChangeMaskForInterest(InterestType interest_type) const;

    void AddTransformHierarchy(TransformHierarchy& hierarchy);
    void RemoveTransformHierarchy(TransformHierarchy& hierarchy);

    void DispatchChanges();

    void RegisterBatchCallback(TransformChangeSystemHandle system, TransformChangeBatchCallback callback);
    void ClearBatchCallbacks(TransformChangeSystemHandle system);

private:
    struct RegisteredSystem
    {
        std::string name;
        InterestType interest_type {0};
        std::vector<TransformChangeBatchCallback> batch_callbacks;
    };

    std::vector<RegisteredSystem> m_Systems;
    std::vector<TransformHierarchy*> m_Hierarchies;
};

TransformChangeDispatch& GetTransformChangeDispatch();
