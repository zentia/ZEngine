#include "PreloadManager.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <chrono>

// ========== PreloadGroup 实现 ==========

PreloadGroup::PreloadGroup(const std::string& group_name)
    : m_GroupName(group_name) {}

PreloadGroup::~PreloadGroup()
{
    Unload();
}

void PreloadGroup::AddAsset(const eastl::string& asset_url, PreloadPriority priority)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (std::find(m_AssetUrls.begin(), m_AssetUrls.end(), asset_url) == m_AssetUrls.end())
    {
        m_AssetUrls.push_back(asset_url);
        m_AssetPriorities[asset_url] = priority;
    }
}

void PreloadGroup::AddAssetWithDependencies(const eastl::string& asset_url,
                                            const eastl::vector<eastl::string>& dependencies,
                                            PreloadPriority priority)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    AddAsset(asset_url, priority);
    m_AssetDependencies[asset_url] = dependencies;
}

void PreloadGroup::StartPreload()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (const auto& asset_url : m_AssetUrls)
    {
        auto priority = m_AssetPriorities.find(asset_url);
        PreloadPriority prio = (priority != m_AssetPriorities.end()) ? priority->second : PreloadPriority::Normal;

        auto deps_it = m_AssetDependencies.find(asset_url);
        if (deps_it != m_AssetDependencies.end())
        {
            GET_SYSTEM(PreloadManager)->PreloadAssetWithDependencies(asset_url, deps_it->second, prio);
        }
        else
        {
            GET_SYSTEM(PreloadManager)->PreloadAsset(asset_url, prio);
        }
    }
}

void PreloadGroup::Unload()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (const auto& asset_url : m_AssetUrls)
    {
        GET_SYSTEM(PreloadManager)->UnloadAsset(asset_url);
    }
}

float PreloadGroup::GetProgress() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_AssetUrls.empty())
    {
        return 1.0f;
    }

    size_t loaded_count = 0;
    for (const auto& asset_url : m_AssetUrls)
    {
        if (GET_SYSTEM(PreloadManager)->IsAssetLoaded(asset_url))
        {
            loaded_count++;
        }
    }

    return static_cast<float>(loaded_count) / static_cast<float>(m_AssetUrls.size());
}

bool PreloadGroup::IsComplete() const
{
    return GetProgress() >= 1.0f;
}

// ========== PreloadManager 实现 ==========
std::vector<std::type_index> PreloadManager::GetDependencies() const
{
    return {};
}

PreloadManager::PreloadManager()
    : m_Initialized(false), m_Shutdown(false), m_Paused(false), m_MaxConcurrentLoads(4), m_ActiveLoadCount(0),
      m_NextRequestId(1)
{
}

PreloadManager::~PreloadManager() {}

bool PreloadManager::Initialize()
{
    if (m_Initialized.load())
    {
        LOG_WARNING(ZPreload, "PreloadManager already initialized");
        return false;
    }

    m_Shutdown.store(false);
    m_Paused.store(false);

#if defined(__EMSCRIPTEN__)
    // Web build is compiled without -pthread; std::thread's constructor
    // unconditionally throws std::system_error("Not supported") under that
    // toolchain. Skip starting the loading worker; on Web all preloads are
    // expected to flow through the asset manager's async (Fetch) path,
    // which is browser-driven and does not need our background thread.
    LOG_INFO(ZPreload, "PreloadManager initialized (Web stub: no worker thread)");
#else
    // 启动加载线程
    m_LoadingThread = std::thread([this]() { LoadingThreadWorker(); });
#endif

    m_Initialized.store(true);
    LOG_INFO(ZPreload, "PreloadManager initialized");
    return true;
}

void PreloadManager::Shutdown()
{
    if (!m_Initialized.load())
    {
        return;
    }

    m_Shutdown.store(true);
    m_LoadingQueue.Stop();

#if !defined(__EMSCRIPTEN__)
    // 等待加载线程结束
    if (m_LoadingThread.joinable())
    {
        m_LoadingThread.join();
    }
#endif

    // 清理所有请求
    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);
        m_Requests.clear();
        m_RequestIdToUrl.clear();
    }

    // 清理所有组
    {
        std::lock_guard<std::mutex> lock(m_GroupsMutex);
        m_Groups.clear();
    }

    m_Initialized.store(false);
    LOG_INFO(ZPreload, "PreloadManager shutdown");
}

uint64_t PreloadManager::PreloadAsset(const eastl::string& asset_url,
                                      PreloadPriority priority,
                                      std::function<void(bool success)> callback)
{
    if (!m_Initialized.load())
    {
        LOG_ERROR(ZPreload, "PreloadManager not initialized");
        return 0;
    }

    // 检查是否已加载
    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);
        auto it = m_Requests.find(asset_url);
        if (it != m_Requests.end())
        {
            auto& request = it->second;
            if (request->status == PreloadStatus::Loaded)
            {
                if (callback)
                {
                    callback(true);
                }
                return 0;  // 已加载，返回0表示无需新请求
            }
            else if (request->status == PreloadStatus::Loading || request->status == PreloadStatus::Pending)
            {
                // 如果已有请求，更新回调
                if (callback)
                {
                    auto old_callback = request->callback;
                    request->callback = [old_callback, callback](bool success) {
                        if (old_callback)
                            old_callback(success);
                        callback(success);
                    };
                }
                return 0;
            }
        }
    }

    // 创建新请求
    auto request = std::make_shared<PreloadRequest>();
    request->asset_url = asset_url;
    request->priority = priority;
    request->status = PreloadStatus::Pending;
    request->dependency_count = 0;
    request->callback = callback;

    uint64_t request_id = GetNextRequestId();

    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);
        m_Requests[asset_url] = request;
        m_RequestIdToUrl[request_id] = asset_url;
    }

    // 添加到加载队列
    TaskPriority task_priority = TaskPriority::normal;
    switch (priority)
    {
        case PreloadPriority::Critical:
            task_priority = TaskPriority::high;
            break;
        case PreloadPriority::High:
            task_priority = TaskPriority::high;
            break;
        case PreloadPriority::Normal:
            task_priority = TaskPriority::normal;
            break;
        case PreloadPriority::Low:
        case PreloadPriority::Background:
            task_priority = TaskPriority::low;
            break;
    }

    m_LoadingQueue.Enqueue(Task(
        [this, asset_url]() {
            if (m_Paused.load())
            {
                return;
            }

            // 检查依赖
            std::shared_ptr<PreloadRequest> request;
            {
                std::lock_guard<std::mutex> lock(m_RequestsMutex);
                auto it = m_Requests.find(asset_url);
                if (it == m_Requests.end() || it->second->status != PreloadStatus::Pending)
                {
                    return;
                }
                request = it->second;
            }

            if (!CheckDependenciesReady(*request))
            {
                // 依赖未就绪，重新入队
                m_LoadingQueue.Enqueue(Task([this, asset_url]() { ProcessLoadingQueue(); }, TaskPriority::normal));
                return;
            }

            // 开始加载
            request->status = PreloadStatus::Loading;
            m_ActiveLoadCount++;

            bool success = LoadAssetInternal(asset_url);

            m_ActiveLoadCount--;
            CompleteRequest(asset_url, success);
        },
        task_priority));

    {
        std::lock_guard<std::mutex> lock(m_StatsMutex);
        m_Statistics.total_requests++;
        m_Statistics.pending_requests++;
    }

    return request_id;
}

uint64_t PreloadManager::PreloadAssetWithDependencies(const eastl::string& asset_url,
                                                      const eastl::vector<eastl::string>& dependencies,
                                                      PreloadPriority priority,
                                                      std::function<void(bool success)> callback)
{
    // 先预载所有依赖
    for (const auto& dep : dependencies)
    {
        PreloadAsset(dep, priority);
    }

    // 创建请求并设置依赖
    auto request = std::make_shared<PreloadRequest>();
    request->asset_url = asset_url;
    request->priority = priority;
    request->status = PreloadStatus::Pending;
    request->dependencies = dependencies;
    request->dependency_count = dependencies.size();
    request->callback = callback;

    uint64_t request_id = GetNextRequestId();

    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);
        m_Requests[asset_url] = request;
        m_RequestIdToUrl[request_id] = asset_url;
    }

    // 添加到队列
    TaskPriority task_priority = (priority == PreloadPriority::Critical) ? TaskPriority::high : TaskPriority::normal;

    m_LoadingQueue.Enqueue(Task([this, asset_url]() { ProcessLoadingQueue(); }, task_priority));

    {
        std::lock_guard<std::mutex> lock(m_StatsMutex);
        m_Statistics.total_requests++;
        m_Statistics.pending_requests++;
    }

    return request_id;
}

bool PreloadManager::LoadAssetSync(const eastl::string& asset_url)
{
    if (IsAssetLoaded(asset_url))
    {
        return true;
    }

    // 同步加载
    return LoadAssetInternal(asset_url);
}

std::shared_ptr<PreloadGroup> PreloadManager::CreateGroup(const std::string& group_name)
{
    std::lock_guard<std::mutex> lock(m_GroupsMutex);

    auto it = m_Groups.find(group_name);
    if (it != m_Groups.end())
    {
        return it->second;
    }

    auto group = std::make_shared<PreloadGroup>(group_name);
    m_Groups[group_name] = group;
    return group;
}

std::shared_ptr<PreloadGroup> PreloadManager::GetGroup(const std::string& group_name)
{
    std::lock_guard<std::mutex> lock(m_GroupsMutex);

    auto it = m_Groups.find(group_name);
    if (it != m_Groups.end())
    {
        return it->second;
    }
    return nullptr;
}

void PreloadManager::RemoveGroup(const std::string& group_name)
{
    std::lock_guard<std::mutex> lock(m_GroupsMutex);

    auto it = m_Groups.find(group_name);
    if (it != m_Groups.end())
    {
        it->second->Unload();
        m_Groups.erase(it);
    }
}

void PreloadManager::PreloadAssets(const std::vector<eastl::string>& asset_urls,
                                   PreloadPriority priority,
                                   std::function<void(size_t loaded, size_t total)> progress_callback)
{
    for (const auto& asset_url : asset_urls)
    {
        PreloadAsset(asset_url, priority, [asset_urls, progress_callback](bool success) {
            if (progress_callback)
            {
                // 统计已加载数量
                size_t loaded = 0;
                for (const auto& url : asset_urls)
                {
                    if (GET_SYSTEM(PreloadManager)->IsAssetLoaded(url))
                    {
                        loaded++;
                    }
                }
                progress_callback(loaded, asset_urls.size());
            }
        });
    }
}

PreloadStatus PreloadManager::GetAssetStatus(const eastl::string& asset_url) const
{
    std::lock_guard<std::mutex> lock(m_RequestsMutex);

    auto it = m_Requests.find(asset_url);
    if (it != m_Requests.end())
    {
        return it->second->status;
    }
    return PreloadStatus::Unloaded;
}

bool PreloadManager::IsAssetLoaded(const eastl::string& asset_url) const
{
    return GetAssetStatus(asset_url) == PreloadStatus::Loaded;
}

float PreloadManager::GetLoadProgress(const eastl::string& asset_url) const
{
    PreloadStatus status = GetAssetStatus(asset_url);
    switch (status)
    {
        case PreloadStatus::Loaded:
            return 1.0f;
        case PreloadStatus::Loading:
            return 0.5f;  // 简化实现，实际可以跟踪更详细的进度
        case PreloadStatus::Failed:
        case PreloadStatus::Unloaded:
            return 0.0f;
        case PreloadStatus::Pending:
            return 0.0f;
    }
    return 0.0f;
}

void PreloadManager::UnloadAsset(const eastl::string& asset_url)
{
    std::lock_guard<std::mutex> lock(m_RequestsMutex);

    auto it = m_Requests.find(asset_url);
    if (it != m_Requests.end())
    {
        it->second->status = PreloadStatus::Unloaded;
        m_Requests.erase(it);
    }

    // 从ID映射中移除
    for (auto id_it = m_RequestIdToUrl.begin(); id_it != m_RequestIdToUrl.end();)
    {
        if (id_it->second == asset_url)
        {
            id_it = m_RequestIdToUrl.erase(id_it);
        }
        else
        {
            ++id_it;
        }
    }
}

void PreloadManager::UnloadAll()
{
    std::lock_guard<std::mutex> lock(m_RequestsMutex);
    m_Requests.clear();
    m_RequestIdToUrl.clear();
}

bool PreloadManager::CancelPreload(uint64_t request_id)
{
    std::lock_guard<std::mutex> lock(m_RequestsMutex);

    auto it = m_RequestIdToUrl.find(request_id);
    if (it != m_RequestIdToUrl.end())
    {
        eastl::string asset_url = it->second;
        auto req_it = m_Requests.find(asset_url);
        if (req_it != m_Requests.end())
        {
            if (req_it->second->status == PreloadStatus::Pending)
            {
                req_it->second->status = PreloadStatus::Unloaded;
                m_Requests.erase(req_it);
                m_RequestIdToUrl.erase(it);
                return true;
            }
        }
    }
    return false;
}

void PreloadManager::PausePreload()
{
    m_Paused.store(true);
}

void PreloadManager::ResumePreload()
{
    m_Paused.store(false);
}

void PreloadManager::SetMaxConcurrentLoads(size_t max_loads)
{
    m_MaxConcurrentLoads.store(max_loads);
}

void PreloadManager::Tick(float delta_time)
{
    // 更新统计信息
    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);

        size_t pending = 0;
        size_t loading = 0;
        size_t loaded = 0;
        size_t failed = 0;

        for (const auto& pair : m_Requests)
        {
            switch (pair.second->status)
            {
                case PreloadStatus::Pending:
                    pending++;
                    break;
                case PreloadStatus::Loading:
                    loading++;
                    break;
                case PreloadStatus::Loaded:
                    loaded++;
                    break;
                case PreloadStatus::Failed:
                    failed++;
                    break;
                default:
                    break;
            }
        }

        std::lock_guard<std::mutex> stats_lock(m_StatsMutex);
        m_Statistics.pending_requests = pending;
        m_Statistics.loading_requests = loading;
        m_Statistics.loaded_requests = loaded;
        m_Statistics.failed_requests = failed;
        m_Statistics.active_loads = m_ActiveLoadCount.load();
    }
}

PreloadManager::Statistics PreloadManager::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    return m_Statistics;
}

void PreloadManager::LoadingThreadWorker()
{
    while (!m_Shutdown.load())
    {
        ProcessLoadingQueue();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps
    }
}

void PreloadManager::ProcessLoadingQueue()
{
    // 检查并发限制
    if (m_ActiveLoadCount.load() >= m_MaxConcurrentLoads.load())
    {
        return;
    }

    if (m_Paused.load())
    {
        return;
    }

    Task task;
    if (m_LoadingQueue.Dequeue(task, 0))
    {
        task.function();
    }
}

bool PreloadManager::LoadAssetInternal(const eastl::string& asset_url)
{
    // 预载系统的核心思想是提前将资源加载到内存
    // 由于 AssetManager 使用模板方法，我们需要根据资源类型加载
    //
    // 实际实现中，预载系统可以：
    // 1. 预读文件到内存缓存
    // 2. 预解析资源元数据
    // 3. 预加载资源依赖
    //
    // 这里提供一个基础实现：验证资源文件存在性
    // 实际使用时，可以根据资源类型扩展加载逻辑

    LOG_INFO(ZPreload, "Loading asset: {}", asset_url.c_str());

    // 检查资源文件是否存在
    std::filesystem::path asset_path = GET_SYSTEM(AssetManager)->GetFullPath(asset_url);
    if (!std::filesystem::exists(asset_path))
    {
        LOG_ERROR(ZPreload, "Asset file not found: {}", asset_path.string().c_str());
        return false;
    }

    // 注意：由于 AssetManager 使用模板，实际加载需要知道资源类型
    // 预载系统的主要作用是：
    // 1. 管理加载队列和优先级
    // 2. 处理依赖关系
    // 3. 提供加载状态查询
    //
    // 实际资源加载仍然由 AssetManager 在需要时完成
    // 预载系统确保资源文件存在且可访问

    // 可以在这里添加：
    // - 文件预读（将文件内容读入内存缓存）
    // - 元数据预解析
    // - 依赖资源预加载

    // 简化实现：验证文件存在即可
    // 实际使用时，可以扩展为真正的资源加载

    return true;
}

bool PreloadManager::CheckDependenciesReady(const PreloadRequest& request) const
{
    if (request.dependencies.empty())
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(m_RequestsMutex);

    for (const auto& dep : request.dependencies)
    {
        auto it = m_Requests.find(dep);
        if (it == m_Requests.end() || it->second->status != PreloadStatus::Loaded)
        {
            return false;
        }
    }
    return true;
}

void PreloadManager::CompleteRequest(const eastl::string& asset_url, bool success)
{
    std::shared_ptr<PreloadRequest> request;
    {
        std::lock_guard<std::mutex> lock(m_RequestsMutex);
        auto it = m_Requests.find(asset_url);
        if (it == m_Requests.end())
        {
            return;
        }
        request = it->second;
    }

    request->status = success ? PreloadStatus::Loaded : PreloadStatus::Failed;

    // 调用回调
    if (request->callback)
    {
        request->callback(success);
    }

    // 通知等待此资源的其他请求
    // 简化实现：重新处理队列中等待依赖的请求
    m_LoadingQueue.Enqueue(Task([this]() { ProcessLoadingQueue(); }, TaskPriority::normal));

    if (success)
    {
        LOG_INFO(ZPreload, "Asset loaded successfully: {}", asset_url.c_str());
    }
    else
    {
        LOG_ERROR(ZPreload, "Failed to load asset: {}", asset_url.c_str());
    }
}

uint64_t PreloadManager::GetNextRequestId()
{
    return m_NextRequestId.fetch_add(1);
}