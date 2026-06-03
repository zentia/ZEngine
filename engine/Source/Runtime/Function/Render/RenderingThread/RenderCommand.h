#pragma once

#include <memory>
#include <mutex>
#include <vector>

// UE-style typed render/RHI command. Each command is a small struct with Execute().
class IRenderCommand
{
public:
    virtual ~IRenderCommand() = default;

    virtual void Execute() = 0;
    virtual const char* GetDebugName() const = 0;
};

using RenderCommandPtr = std::unique_ptr<IRenderCommand>;

// Thread-safe pending command list. Submit* steals the batch and runs it on the worker.
class RenderCommandQueue
{
public:
    void Enqueue(RenderCommandPtr command);

    // Move pending commands out for execution on the consumer thread.
    std::vector<RenderCommandPtr> StealCommands();

    void Clear();

    bool IsEmpty() const;

private:
    mutable std::mutex m_Mutex;
    std::vector<RenderCommandPtr> m_Pending;
};
