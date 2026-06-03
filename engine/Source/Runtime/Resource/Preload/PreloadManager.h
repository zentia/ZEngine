#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Thread/TaskQueue.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 预载优先级（参考 Unity Addressables 和 UE Streaming）
enum class PreloadPriority : uint8_t
{
    Critical = 0,   // 关键资源（必须立即加载）
    High = 1,       // 高优先级（重要资源）
    Normal = 2,     // 普通优先级（常规资源）
    Low = 3,        // 低优先级（后台预载）
    Background = 4  // 后台加载（最低优先级）
};

// 预载状态
enum class PreloadStatus : uint8_t
{
    Pending,  // 等待加载
    Loading,  // 正在加载
    Loaded,   // 已加载
    Failed,   // 加载失败
    Unloaded  // 已卸载
};

// 预载请求
struct PreloadRequest
{
    eastl::string asset_url;                     // 资源路径
    PreloadPriority priority;                    // 优先级
    PreloadStatus status;                        // 状态
    std::atomic<size_t> dependency_count;        // 依赖计数（等待依赖加载完成）
    eastl::vector<eastl::string> dependencies;   // 依赖的资源列表
    std::function<void(bool success)> callback;  // 完成回调

    PreloadRequest()
        : priority(PreloadPriority::Normal), status(PreloadStatus::Pending), dependency_count(0) {}
};

// 预载组（类似 Unity Addressables Group）
class PreloadGroup
{
public:
    PreloadGroup(const std::string& group_name);
    ~PreloadGroup();

    // 添加资源到预载组
    void AddAsset(const eastl::string& asset_url, PreloadPriority priority = PreloadPriority::Normal);

    // 添加资源并指定依赖
    void AddAssetWithDependencies(const eastl::string& asset_url,
                                  const eastl::vector<eastl::string>& dependencies,
                                  PreloadPriority priority = PreloadPriority::Normal);

    // 开始预载组中的所有资源
    void StartPreload();

    // 卸载组中的所有资源
    void Unload();

    // 获取加载进度 [0.0, 1.0]
    float GetProgress() const;

    // 检查是否全部加载完成
    bool IsComplete() const;

    // 获取组名
    const std::string& GetName() const { return m_GroupName; }

    // 获取资源列表
    const std::vector<eastl::string>& getAssets() const { return m_AssetUrls; }

private:
    std::string m_GroupName;
    std::vector<eastl::string> m_AssetUrls;
    eastl::unordered_map<eastl::string, PreloadPriority> m_AssetPriorities;
    eastl::unordered_map<eastl::string, eastl::vector<eastl::string>> m_AssetDependencies;
    mutable std::mutex m_Mutex;
};

// 预载管理器（核心类）
class PreloadManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "PreloadManager"; }
    std::vector<std::type_index> GetDependencies() const override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }

    PreloadManager();
    ~PreloadManager();

    // 初始化预载系统
    bool Initialize() override;

    // 关闭预载系统
    void Shutdown() override;

    // ========== 单资源预载接口 ==========

    /**
     * @brief 预载单个资源（异步）
     * @param asset_url 资源路径
     * @param priority 优先级
     * @param callback 完成回调（可选）
     * @return 请求ID（可用于取消或查询状态）
     */
    uint64_t PreloadAsset(const eastl::string& asset_url,
                          PreloadPriority priority = PreloadPriority::Normal,
                          std::function<void(bool success)> callback = nullptr);

    /**
     * @brief 预载资源并指定依赖
     */
    uint64_t PreloadAssetWithDependencies(const eastl::string& asset_url,
                                          const eastl::vector<eastl::string>& dependencies,
                                          PreloadPriority priority = PreloadPriority::Normal,
                                          std::function<void(bool success)> callback = nullptr);

    /**
     * @brief 同步加载资源（阻塞直到完成）
     */
    bool LoadAssetSync(const eastl::string& asset_url);

    // ========== 预载组接口 ==========

    /**
     * @brief 创建预载组
     */
    std::shared_ptr<PreloadGroup> CreateGroup(const std::string& group_name);

    /**
     * @brief 获取预载组
     */
    std::shared_ptr<PreloadGroup> GetGroup(const std::string& group_name);

    /**
     * @brief 删除预载组
     */
    void RemoveGroup(const std::string& group_name);

    // ========== 批量预载接口 ==========

    /**
     * @brief 批量预载资源
     */
    void PreloadAssets(const std::vector<eastl::string>& asset_urls,
                       PreloadPriority priority = PreloadPriority::Normal,
                       std::function<void(size_t loaded, size_t total)> progress_callback = nullptr);

    // ========== 状态查询接口 ==========

    /**
     * @brief 获取资源加载状态
     */
    PreloadStatus GetAssetStatus(const eastl::string& asset_url) const;

    /**
     * @brief 检查资源是否已加载
     */
    bool IsAssetLoaded(const eastl::string& asset_url) const;

    /**
     * @brief 获取加载进度 [0.0, 1.0]
     */
    float GetLoadProgress(const eastl::string& asset_url) const;

    // ========== 卸载接口 ==========

    /**
     * @brief 卸载资源
     */
    void UnloadAsset(const eastl::string& asset_url);

    /**
     * @brief 卸载所有资源
     */
    void UnloadAll();

    // ========== 控制接口 ==========

    /**
     * @brief 取消预载请求
     */
    bool CancelPreload(uint64_t request_id);

    /**
     * @brief 暂停预载
     */
    void PausePreload();

    /**
     * @brief 恢复预载
     */
    void ResumePreload();

    /**
     * @brief 设置最大并发加载数
     */
    void SetMaxConcurrentLoads(size_t max_loads);

    // ========== 更新接口 ==========

    /**
     * @brief 每帧更新（需要在主线程调用）
     */
    void Tick(float delta_time);

    // ========== 统计信息 ==========

    struct Statistics
    {
        size_t total_requests = 0;
        size_t pending_requests = 0;
        size_t loading_requests = 0;
        size_t loaded_requests = 0;
        size_t failed_requests = 0;
        size_t active_loads = 0;
    };

    Statistics GetStatistics() const;

private:
    // 加载线程工作函数
    void LoadingThreadWorker();

    // 处理加载队列
    void ProcessLoadingQueue();

    // 加载单个资源
    bool LoadAssetInternal(const eastl::string& asset_url);

    // 检查依赖是否已加载
    bool CheckDependenciesReady(const PreloadRequest& request) const;

    // 完成加载请求
    void CompleteRequest(const eastl::string& asset_url, bool success);

    // 获取下一个请求ID
    uint64_t GetNextRequestId();

    // 成员变量
    std::atomic<bool> m_Initialized;
    std::atomic<bool> m_Shutdown;
    std::atomic<bool> m_Paused;

    // 请求管理
    eastl::unordered_map<eastl::string, std::shared_ptr<PreloadRequest>> m_Requests;
    std::unordered_map<uint64_t, eastl::string> m_RequestIdToUrl;
    mutable std::mutex m_RequestsMutex;

    // 预载组
    std::unordered_map<std::string, std::shared_ptr<PreloadGroup>> m_Groups;
    std::mutex m_GroupsMutex;

    // 加载队列（按优先级排序）
    TaskQueue m_LoadingQueue;
    std::thread m_LoadingThread;

    // 统计信息
    mutable std::mutex m_StatsMutex;
    Statistics m_Statistics;

    // 配置
    std::atomic<size_t> m_MaxConcurrentLoads;
    std::atomic<size_t> m_ActiveLoadCount;

    // 请求ID生成器
    std::atomic<uint64_t> m_NextRequestId;
};