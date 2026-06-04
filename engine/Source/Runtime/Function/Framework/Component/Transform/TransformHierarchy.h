#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/Framework/Component/Transform/TransformHierarchyTypes.h"

class Transform;

TransformHierarchy* CreateTransformHierarchy(uint32_t transform_capacity);
void DestroyTransformHierarchy(TransformHierarchy* hierarchy);

void AllocateTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last);
void InsertTransformThreadAfter(TransformHierarchy& hierarchy, uint32_t index, uint32_t thread_first, uint32_t thread_last);
void DetachTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last);
void FreeTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last);

void AddTransformSubhierarchy(TransformHierarchy& src_hierarchy,
                              uint32_t src_index,
                              TransformHierarchy& dst_hierarchy,
                              uint32_t& dst_first,
                              uint32_t& dst_last,
                              TransformChangeSystemMask change_mask = 0);
void CopyTransformSubhierarchy(TransformHierarchy& src_hierarchy,
                               uint32_t src_index,
                               TransformHierarchy& dst_hierarchy,
                               TransformChangeSystemMask change_mask = 0);
void UpdateDeepChildCountUpwards(TransformHierarchy& hierarchy, int32_t index, int32_t added_node_count);

bool GrowTransformHierarchyCapacity(TransformHierarchy* hierarchy, uint32_t min_capacity);

void UpdateTransformAccessors(TransformHierarchy& hierarchy, uint32_t index);

TransformAccessReadOnly GetParent(TransformAccessReadOnly access);
uint32_t GetDeepChildCount(const TransformHierarchy& hierarchy, uint32_t index);
uint32_t GetDeepChildCount(TransformAccessReadOnly access);

const TransformTRS& GetLocalTRS(TransformAccessReadOnly access);
TransformTRS& GetLocalTRSWritable(TransformAccess access);

Matrix4x4 CalculateGlobalMatrix(TransformAccessReadOnly access);
Vector3d CalculateGlobalPositionD(TransformAccessReadOnly access);
Vector3 CalculateGlobalPosition(TransformAccessReadOnly access);
Quaternion CalculateGlobalRotation(TransformAccessReadOnly access);
Vector3 CalculateGlobalScaleLossy(TransformAccessReadOnly access);

Vector3 TransformPoint(TransformAccessReadOnly access, const Vector3& local_point);
Vector3 TransformDirection(TransformAccessReadOnly access, const Vector3& local_direction);
Vector3 TransformVector(TransformAccessReadOnly access, const Vector3& local_vector);
Vector3 InverseTransformPoint(TransformAccessReadOnly access, const Vector3& world_point);
Vector3 InverseTransformDirection(TransformAccessReadOnly access, const Vector3& world_direction);
Vector3 InverseTransformVector(TransformAccessReadOnly access, const Vector3& world_vector);

namespace TransformInternal
{
    void InitLocalTRS(TransformAccess access,
                      const Vector3d& position,
                      const Quaternion& rotation,
                      const Vector3& scale);

    void OnTransformChangedMask(TransformAccess access,
                                TransformChangeSystemMask local_only_mask,
                                TransformChangeSystemMask common_mask,
                                TransformChangeSystemMask children_only_mask);

    void OnLocalPositionChanged(TransformAccess access);
    void OnLocalRotationChanged(TransformAccess access);
    void OnLocalScaleChanged(TransformAccess access);
    void OnLocalTRSChanged(TransformAccess access);
}
