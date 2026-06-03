#include "RenderCommand.h"

void RenderCommandQueue::Enqueue(RenderCommandPtr command)
{
    if (!command)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pending.push_back(std::move(command));
}

std::vector<RenderCommandPtr> RenderCommandQueue::StealCommands()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<RenderCommandPtr> stolen;
    stolen.swap(m_Pending);
    return stolen;
}

void RenderCommandQueue::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pending.clear();
}

bool RenderCommandQueue::IsEmpty() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Pending.empty();
}
