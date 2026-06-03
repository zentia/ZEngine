#include "ThreadManager.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Profiler/InsightsTrace.h"  // ZEngine Insights track labels

#include <chrono>
#include <thread>

#ifdef Z_PLATFORM_WINDOWS
    #include <processthreadsapi.h>
    #include <windows.h>
#elif defined(Z_PLATFORM_LINUX) || defined(Z_PLATFORM_MACOS)
    #include <pthread.h>
#endif

// -----------------------------------------------------------------------------
// Web (Emscripten) ThreadManager strategy
// -----------------------------------------------------------------------------
// We build the WebLauncher *without* -pthread, because enabling Emscripten
// pthreads requires SharedArrayBuffer, which in turn forces the page to be
// served with COOP/COEP cross-origin isolation headers — a deployment
// constraint we don't want to push onto bring-up.
//
// In a non-pthread Emscripten build, std::thread's constructor immediately
// throws std::system_error("thread constructor failed: Not supported")
// (libc++ checks _LIBCPP_HAS_NO_THREADS / __EMSCRIPTEN_PTHREADS__). That was
// the abort observed in the browser console during InitializeAll().
//
// Strategy: keep the public ThreadManager API intact, but on Web run every
// task synchronously on the main (game) thread:
//   * Initialize()/Shutdown() do NOT spawn std::thread.
//   * All thread IDs collapse to the game thread id, so IsOnRenderThread()
//     and IsOnRHIThread() return true on the main thread (matches the
//     reality that WebGL2 / DOM access *must* happen on the main thread).
//   * EnqueueTask() executes the task inline.
//   * WaitForThread() / WaitForAllThreads() are no-ops.
//
// This mirrors how PreloadManager / PhysicsManager (Jolt) and BqLog are
// stubbed for Web elsewhere in the engine.
// -----------------------------------------------------------------------------

ThreadManager::ThreadManager()
    : m_RenderQueue(std::make_unique<TaskQueue>()), m_RhiQueue(std::make_unique<TaskQueue>())
{
    m_GameThreadId = std::this_thread::get_id();
    // Label the main/game thread in the ZEngine Insights timeline.
    ZEngine::Insights::InsightsTrace::Get().SetThreadName("GameThread");
}

ThreadManager::~ThreadManager()
{
    Shutdown();
}

bool ThreadManager::Initialize()
{
    if (m_Initialized.load())
    {
        LOG_WARNING(ZThread, "ThreadManager already initialized");
        return false;
    }

    m_Shutdown.store(false);

#if defined(__EMSCRIPTEN__)
    // Web: no worker threads; collapse every "thread" onto the game thread.
    // See file header for the rationale.
    m_RenderThreadId = m_GameThreadId;
    m_RhiThreadId = m_GameThreadId;
    m_Initialized.store(true);
    LOG_INFO(ZThread, "ThreadManager initialized (Web single-threaded mode)");
    return true;
#else
    // 启动渲染线程
    m_RenderThread = std::thread([this]() { ThreadWorker(ThreadType::render, "RenderThread"); });

    // 启动 RHI 线程
    m_RhiThread = std::thread([this]() { ThreadWorker(ThreadType::rhi, "RHIThread"); });

    // 等待线程启动并获取线程 ID
    // 使用一个简单的任务来确保线程已经启动
    m_RenderQueue->Enqueue(Task(
        [this]() {
            m_RenderThreadId = std::this_thread::get_id();
            SetThreadName("RenderThread");
        },
        TaskPriority::high));

    m_RhiQueue->Enqueue(Task(
        [this]() {
            m_RhiThreadId = std::this_thread::get_id();
            SetThreadName("RHIThread");
        },
        TaskPriority::high));

    // 等待线程初始化完成
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    m_Initialized.store(true);
    LOG_INFO(ZThread, "ThreadManager initialized successfully");
    return true;
#endif
}

void ThreadManager::Shutdown()
{
    if (!m_Initialized.load())
    {
        return;
    }

    m_Shutdown.store(true);

    // 停止所有队列
    m_RenderQueue->Stop();
    m_RhiQueue->Stop();

#if !defined(__EMSCRIPTEN__)
    // 等待线程结束
    if (m_RenderThread.joinable())
    {
        m_RenderThread.join();
    }

    if (m_RhiThread.joinable())
    {
        m_RhiThread.join();
    }
#endif

    m_Initialized.store(false);
    LOG_INFO(ZThread, "ThreadManager shutdown");
}

void ThreadManager::EnqueueTask(ThreadType thread_type, std::function<void()> task, TaskPriority priority)
{
    if (!m_Initialized.load())
    {
        LOG_ERROR(ZThread, "ThreadManager not initialized");
        return;
    }

#if defined(__EMSCRIPTEN__)
    // Web: there are no worker threads to dispatch to. WebGL2 and the DOM
    // are bound to the main (game) thread anyway, so running the task
    // synchronously here is the correct semantic for "run on render/rhi".
    (void)priority;
    if (task)
    {
        task();
    }
    return;
#else
    TaskQueue* queue = nullptr;
    switch (thread_type)
    {
        case ThreadType::render:
            queue = m_RenderQueue.get();
            break;
        case ThreadType::rhi:
            queue = m_RhiQueue.get();
            break;
        case ThreadType::game:
            // 游戏线程直接执行
            task();
            return;
        default:
            LOG_ERROR(ZThread, "Unknown thread type");
            return;
    }

    if (queue)
    {
        queue->Enqueue(Task(std::move(task), priority));
    }
#endif
}

void ThreadManager::EnqueueRenderTask(std::function<void()> task, TaskPriority priority)
{
    EnqueueTask(ThreadType::render, std::move(task), priority);
}

void ThreadManager::EnqueueRHITask(std::function<void()> task, TaskPriority priority)
{
    EnqueueTask(ThreadType::rhi, std::move(task), priority);
}

void ThreadManager::WaitForThread(ThreadType thread_type)
{
    if (!m_Initialized.load())
    {
        return;
    }

#if defined(__EMSCRIPTEN__)
    // Web: tasks are run synchronously inside enqueueTask, so by the time
    // the caller reaches WaitForThread() there is nothing pending. A busy
    // sleep loop here would also lock up the browser main thread.
    (void)thread_type;
    return;
#else
    TaskQueue* queue = nullptr;
    switch (thread_type)
    {
        case ThreadType::render:
            queue = m_RenderQueue.get();
            break;
        case ThreadType::rhi:
            queue = m_RhiQueue.get();
            break;
        case ThreadType::game:
            return;
        default:
            return;
    }

    if (queue)
    {
        // 等待队列为空
        while (!queue->IsEmpty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
#endif
}

void ThreadManager::WaitForAllThreads()
{
    WaitForThread(ThreadType::render);
    WaitForThread(ThreadType::rhi);
}

bool ThreadManager::IsOnThread(ThreadType thread_type) const
{
    auto current_id = std::this_thread::get_id();

    switch (thread_type)
    {
        case ThreadType::game:
            return current_id == m_GameThreadId;
        case ThreadType::render:
            return current_id == m_RenderThreadId;
        case ThreadType::rhi:
            return current_id == m_RhiThreadId;
        default:
            return false;
    }
}

bool ThreadManager::IsOnRenderThread() const
{
    return IsOnThread(ThreadType::render);
}

bool ThreadManager::IsOnRHIThread() const
{
    return IsOnThread(ThreadType::rhi);
}

bool ThreadManager::IsOnGameThread() const
{
    return IsOnThread(ThreadType::game);
}

TaskQueue* ThreadManager::GetTaskQueue(ThreadType thread_type)
{
    switch (thread_type)
    {
        case ThreadType::render:
            return m_RenderQueue.get();
        case ThreadType::rhi:
            return m_RhiQueue.get();
        default:
            return nullptr;
    }
}

void ThreadManager::ThreadWorker(ThreadType thread_type, const std::string& thread_name)
{
    TaskQueue* queue = nullptr;
    switch (thread_type)
    {
        case ThreadType::render:
            queue = m_RenderQueue.get();
            break;
        case ThreadType::rhi:
            queue = m_RhiQueue.get();
            break;
        default:
            return;
    }

    if (!queue)
    {
        return;
    }

    Task task;
    while (!m_Shutdown.load())
    {
        if (queue->Dequeue(task, 100))  // 100ms 超时，用于检查 shutdown 标志
        {
            try
            {
                if (task.function)
                {
                    task.function();
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR(ZThread, "Exception in {}: {}", thread_name, Encoding::GetExceptionMessage(e));
            }
            catch (...)
            {
                LOG_ERROR(ZThread, "Unknown exception in {}", thread_name);
            }
        }
    }

    // 处理剩余任务
    while (queue->TryDequeue(task))
    {
        try
        {
            if (task.function)
            {
                task.function();
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(ZThread, "Exception in {} during shutdown: {}", thread_name, Encoding::GetExceptionMessage(e));
        }
        catch (...)
        {
            LOG_ERROR(ZThread, "Unknown exception in {} during shutdown", thread_name);
        }
    }
}

#ifdef Z_PLATFORM_WINDOWS
// 辅助函数：使用结构化异常处理设置线程名称（VS 调试器）
// 此函数必须不包含任何需要析构的 C++ 对象，以便使用 __try/__except
static void setThreadNameViaException(DWORD thread_id, LPCSTR thread_name)
{
    #pragma Pack(push, 8)
    struct ThreadNameInfo
    {
        DWORD dwType;
        LPCSTR szName;
        DWORD dwThreadID;
        DWORD dwFlags;
    };
    #pragma Pack(pop)

    ThreadNameInfo info;
    info.dwType = 0x1000;
    info.szName = thread_name;
    info.dwThreadID = thread_id;
    info.dwFlags = 0;

    __try
    {
        RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif

void ThreadManager::SetThreadName(const std::string& name)
{
    // Mirror the OS thread name into the ZEngine Insights timeline (this runs ON
    // the thread being named -- see the render / RHI worker entry points).
    ZEngine::Insights::InsightsTrace::Get().SetThreadName(name.c_str());

#ifdef Z_PLATFORM_WINDOWS
    // Windows 10 version 1607+ 支持 SetThreadDescription
    typedef HRESULT(WINAPI * SetThreadDescriptionFunc)(HANDLE, PCWSTR);
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32)
    {
        SetThreadDescriptionFunc setThreadDescription =
            (SetThreadDescriptionFunc)GetProcAddress(kernel32, "SetThreadDescription");
        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(GetCurrentThread(), wname.c_str());
        }
    }

    // 使用旧的方式设置线程名称（VS 调试器）
    // 通过辅助函数调用，避免在包含 C++ 对象的函数中使用 __try
    setThreadNameViaException(GetCurrentThreadId(), name.c_str());
#elif defined(Z_PLATFORM_LINUX) || defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_OHOS)
    // POSIX-like platforms use pthread_setname_np
    #if defined(__linux__) || defined(__OHOS__)
    pthread_setname_np(pthread_self(), name.c_str());
    #elif defined(__APPLE__)
    pthread_setname_np(name.c_str());
    #endif
#endif
    (void)name;  // 避免未使用参数警告
}