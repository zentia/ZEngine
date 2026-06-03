#pragma once

#include "DebugDrawGroup.h"

class DebugDrawContext
{
public:
    std::vector<DebugDrawGroup*> m_DebugDrawGroups;
    DebugDrawGroup* TryGetOrCreateDebugDrawGroup(const std::string& name);
    void clear();
    void Tick(float delta_time);

private:
    std::mutex m_Mutex;
    void RemoveDeadPrimitives(float delta_time);
};