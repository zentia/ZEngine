#include "DebugDrawContext.h"

DebugDrawGroup* DebugDrawContext::TryGetOrCreateDebugDrawGroup(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_Mutex);

    size_t debug_draw_group_count = m_DebugDrawGroups.size();
    for (size_t debug_draw_group_index = 0; debug_draw_group_index < debug_draw_group_count; debug_draw_group_index++)
    {
        DebugDrawGroup* debug_draw_group = m_DebugDrawGroups[debug_draw_group_index];
        if (debug_draw_group->GetName() == name)
        {
            return debug_draw_group;
        }
    }

    DebugDrawGroup* new_debug_draw_group = new DebugDrawGroup;
    new_debug_draw_group->Initialize();
    new_debug_draw_group->SetName(name);
    m_DebugDrawGroups.push_back(new_debug_draw_group);

    return new_debug_draw_group;
}

void DebugDrawContext::clear()
{
    std::lock_guard<std::mutex> guard(m_Mutex);

    size_t debug_draw_group_count = m_DebugDrawGroups.size();
    for (size_t debug_draw_group_index = 0; debug_draw_group_index < debug_draw_group_count; debug_draw_group_index++)
    {
        delete m_DebugDrawGroups[debug_draw_group_index];
    }

    m_DebugDrawGroups.clear();
}

void DebugDrawContext::Tick(float delta_time)
{
    RemoveDeadPrimitives(delta_time);
}

void DebugDrawContext::RemoveDeadPrimitives(float delta_time)
{
    std::lock_guard<std::mutex> guard(m_Mutex);

    size_t debug_draw_group_count = m_DebugDrawGroups.size();
    for (size_t debug_draw_group_index = 0; debug_draw_group_index < debug_draw_group_count; debug_draw_group_index++)
    {
        if (m_DebugDrawGroups[debug_draw_group_index] == nullptr)
            continue;
        m_DebugDrawGroups[debug_draw_group_index]->RemoveDeadPrimitives(delta_time);
    }
}