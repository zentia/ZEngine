#include "Runtime/Function/Network/NetworkSystem.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <iterator>
#include <utility>

#if defined(ZENGINE_NETWORK_HAS_CURL)
    #include <curl/curl.h>
#endif

namespace
{
    /// How long the worker parks in curl_multi_wait() when nothing is
    /// happening. Bounds both shutdown latency and the delay before a freshly
    /// submitted request is picked up.
    constexpr int kWorkerWaitMilliseconds = 50;
}  // namespace

// =============================================================================
// IEngineSystem
// =============================================================================

std::size_t NetworkSystem::GetAbiSize()
{
    return sizeof(NetworkSystem);
}

bool NetworkSystem::Initialize()
{
    if (m_Initialized.load(std::memory_order_acquire))
    {
        return true;
    }

#if defined(ZENGINE_NETWORK_HAS_CURL)
    // CURL_GLOBAL_DEFAULT pulls in SSL + Win32 socket init. Not thread-safe
    // against other libcurl calls, so it happens before the worker exists.
    const CURLcode global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (global_result != CURLE_OK)
    {
        LOG_ERROR(ZEngine, "NetworkSystem: curl_global_init failed: {}; HTTP/WebSocket disabled",
                  curl_easy_strerror(global_result));
        // Deliberately NOT returning false: a transport that cannot start is a
        // degraded engine, not a failed boot. Everything that follows reports
        // NetworkStatus::Unsupported (see IsBackendAvailable()).
        m_StopRequested.store(false, std::memory_order_release);
        m_Initialized.store(true, std::memory_order_release);
        return true;
    }

    m_Multi = curl_multi_init();
    if (m_Multi == nullptr)
    {
        LOG_ERROR(ZEngine, "NetworkSystem: curl_multi_init failed; HTTP/WebSocket disabled");
        curl_global_cleanup();
        m_StopRequested.store(false, std::memory_order_release);
        m_Initialized.store(true, std::memory_order_release);
        return true;
    }
#endif

    m_StopRequested.store(false, std::memory_order_release);
    m_Initialized.store(true, std::memory_order_release);

#if defined(ZENGINE_NETWORK_HAS_CURL)
    m_Worker = std::thread(&NetworkSystem::WorkerMain, this);
    m_BackendReady.store(true, std::memory_order_release);
    LOG_INFO(ZEngine, "NetworkSystem: initialized (libcurl backend)");
#else
    LOG_INFO(ZEngine, "NetworkSystem: initialized without libcurl; HTTP/WebSocket unavailable");
#endif
    return true;
}

void NetworkSystem::Shutdown()
{
    if (!m_Initialized.load(std::memory_order_acquire))
    {
        return;
    }

    CloseAllWebSockets();

    m_StopRequested.store(true, std::memory_order_release);
    if (m_Worker.joinable())
    {
        m_Worker.join();
    }

#if defined(ZENGINE_NETWORK_HAS_CURL)
    {
        // Any request the worker did not get to finish still owns a CURL handle
        // registered on the multi; it has to come off before curl_multi_cleanup.
        std::lock_guard<std::mutex> lock(m_ActiveMutex);
        for (auto& [handle, active] : m_Active)
        {
            if (active.request != nullptr && m_Multi != nullptr)
            {
                curl_multi_remove_handle(m_Multi, active.request->GetCurlHandle());
            }
        }
        m_Active.clear();
        m_HandleByCurl.clear();
    }

    if (m_Multi != nullptr)
    {
        curl_multi_cleanup(m_Multi);
        m_Multi = nullptr;
    }
#endif

    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_PendingSubmits.clear();
        m_PendingCancels.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_CompletionMutex);
        m_Completions.clear();
    }

#if defined(ZENGINE_NETWORK_HAS_CURL)
    curl_global_cleanup();
#endif

    m_BackendReady.store(false, std::memory_order_release);
    m_Initialized.store(false, std::memory_order_release);
    LOG_INFO(ZEngine, "NetworkSystem: shut down");
}

// =============================================================================
// Worker thread
// =============================================================================

void NetworkSystem::WorkerMain()
{
#if defined(ZENGINE_NETWORK_HAS_CURL)
    while (!m_StopRequested.load(std::memory_order_acquire))
    {
        DrainCancels();
        DrainSubmits();

        int still_running = 0;
        curl_multi_perform(m_Multi, &still_running);

        std::vector<Completion> ready;
        CURLMsg*                message = nullptr;
        int                     messages_left = 0;
        while ((message = curl_multi_info_read(m_Multi, &messages_left)) != nullptr)
        {
            if (message->msg != CURLMSG_DONE || message->easy_handle == nullptr)
            {
                continue;
            }

            CURL*         easy   = message->easy_handle;
            ActiveRequest active;
            HttpHandle    finished_handle = kInvalidHttpHandle;
            {
                std::lock_guard<std::mutex> lock(m_ActiveMutex);
                const auto                  curl_it = m_HandleByCurl.find(easy);
                if (curl_it != m_HandleByCurl.end())
                {
                    finished_handle = curl_it->second;
                    const auto active_it = m_Active.find(curl_it->second);
                    if (active_it != m_Active.end())
                    {
                        active = std::move(active_it->second);
                        m_Active.erase(active_it);
                    }
                    m_HandleByCurl.erase(curl_it);
                }
            }

            curl_multi_remove_handle(m_Multi, easy);

            if (active.request != nullptr)
            {
                active.request->OnTransferFinished(static_cast<int>(message->data.result));
                ready.push_back(Completion {finished_handle, active.request->GetResponse(),
                                            std::move(active.on_complete)});
            }
        }

        if (!ready.empty())
        {
            std::lock_guard<std::mutex> lock(m_CompletionMutex);
            m_Completions.insert(m_Completions.end(),
                                 std::make_move_iterator(ready.begin()),
                                 std::make_move_iterator(ready.end()));
        }

        // Park until a socket wakes up or the timeout expires. No extra fds to
        // monitor: the worker has nothing of its own to wait on.
        int wakeup_count = 0;
        curl_multi_wait(m_Multi, nullptr, 0, kWorkerWaitMilliseconds, &wakeup_count);
    }
#endif
}

void NetworkSystem::DrainSubmits()
{
    std::vector<PendingSubmit> taken;
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        if (m_PendingSubmits.empty())
        {
            return;
        }
        taken.swap(m_PendingSubmits);
    }

    std::lock_guard<std::mutex> active_lock(m_ActiveMutex);
    for (auto& pending : taken)
    {
        if (pending.request.request == nullptr)
        {
            continue;
        }
#if defined(ZENGINE_NETWORK_HAS_CURL)
        curl_multi_add_handle(m_Multi, pending.request.request->GetCurlHandle());
        m_HandleByCurl[pending.request.request->GetCurlHandle()] = pending.handle;
#endif
        m_Active[pending.handle] = std::move(pending.request);
    }
}

void NetworkSystem::DrainCancels()
{
    std::vector<HttpHandle> taken;
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        if (m_PendingCancels.empty())
        {
            return;
        }
        taken.swap(m_PendingCancels);
    }

    std::lock_guard<std::mutex> active_lock(m_ActiveMutex);
    for (const HttpHandle handle : taken)
    {
        const auto it = m_Active.find(handle);
        if (it == m_Active.end())
        {
            continue;
        }
#if defined(ZENGINE_NETWORK_HAS_CURL)
        const auto curl_it = m_HandleByCurl.find(it->second.request->GetCurlHandle());
        curl_multi_remove_handle(m_Multi, it->second.request->GetCurlHandle());
        if (curl_it != m_HandleByCurl.end())
        {
            m_HandleByCurl.erase(curl_it);
        }
#endif
        it->second.request->Cancel();
        // Callback deliberately dropped: the caller asked for the request to go
        // away, not to be told it did.
        m_Active.erase(it);
    }
}

void NetworkSystem::DrainCompletions(std::vector<Completion>& out_ready)
{
    std::lock_guard<std::mutex> lock(m_CompletionMutex);
    if (m_Completions.empty())
    {
        return;
    }
    out_ready.swap(m_Completions);
}

// =============================================================================
// Asynchronous HTTP
// =============================================================================

HttpHandle NetworkSystem::SendAsync(const HttpRequestDesc& desc, HttpCompletionCallback on_complete)
{
    if (!m_Initialized.load(std::memory_order_acquire))
    {
        LOG_WARNING(ZEngine, "NetworkSystem::SendAsync before Initialize(); request dropped");
        return kInvalidHttpHandle;
    }

    if (!IsBackendAvailable())
    {
        // No transport (curl-less build, or curl failed to start). Still report
        // the failure through the completion queue: a caller that waits on its
        // callback would otherwise block forever on a request we silently
        // swallowed.
        LOG_WARNING(ZEngine, "NetworkSystem: no transport available; '{}' not sent", desc.url);
        const HttpHandle handle = m_NextHandle.fetch_add(1, std::memory_order_relaxed);
        HttpResponse     response;
        response.status = NetworkStatus::Unsupported;
        response.error  = "no network transport available";
        std::lock_guard<std::mutex> lock(m_CompletionMutex);
        m_Completions.push_back(Completion {handle, std::move(response), std::move(on_complete)});
        return handle;
    }

    const HttpHandle handle = m_NextHandle.fetch_add(1, std::memory_order_relaxed);
    auto             request = std::make_shared<HttpRequest>();

    if (!request->Start(desc))
    {
        // Start() already filled in the failure. Route it through the normal
        // completion queue rather than invoking the callback re-entrantly: the
        // caller must never see its callback run inside SendAsync().
        LOG_WARNING(ZEngine, "NetworkSystem: request '{}' rejected: {}", desc.url,
                    request->GetResponse().error);
        std::lock_guard<std::mutex> lock(m_CompletionMutex);
        m_Completions.push_back(
            Completion {handle, request->GetResponse(), std::move(on_complete)});
        return handle;
    }

    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_PendingSubmits.push_back(PendingSubmit {handle,
                                                  ActiveRequest {std::move(request), std::move(on_complete)}});
    }
    return handle;
}

bool NetworkSystem::IsHandlePending(HttpHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_SubmitMutex);
    return std::any_of(m_PendingSubmits.begin(), m_PendingSubmits.end(),
                       [handle](const PendingSubmit& pending) { return pending.handle == handle; });
}

bool NetworkSystem::IsRequestDone(HttpHandle handle) const
{
    if (handle == kInvalidHttpHandle)
    {
        return true;
    }
    if (IsHandlePending(handle))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    return m_Active.find(handle) == m_Active.end();
}

HttpProgress NetworkSystem::GetRequestProgress(HttpHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    const auto                  it = m_Active.find(handle);
    if (it == m_Active.end() || it->second.request == nullptr)
    {
        return HttpProgress();
    }
    return it->second.request->GetProgress();
}

bool NetworkSystem::CancelRequest(HttpHandle handle)
{
    if (handle == kInvalidHttpHandle)
    {
        return false;
    }

    // Still queued? Drop it before the worker ever sees it.
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        const auto                  it = std::find_if(m_PendingSubmits.begin(), m_PendingSubmits.end(),
                                                     [handle](const PendingSubmit& pending) {
                                                         return pending.handle == handle;
                                                     });
        if (it != m_PendingSubmits.end())
        {
            m_PendingSubmits.erase(it);
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_PendingCancels.push_back(handle);
    }

    // Last chance: the request may already have finished and be sitting in the
    // completion queue waiting for Pump(). Drop it there too, so a cancelled
    // request never reaches its callback.
    {
        std::lock_guard<std::mutex> lock(m_CompletionMutex);
        const auto                  it = std::find_if(
            m_Completions.begin(), m_Completions.end(),
            [handle](const Completion& completion) { return completion.handle == handle; });
        if (it != m_Completions.end())
        {
            m_Completions.erase(it);
        }
    }
    return true;
}

void NetworkSystem::CancelAllRequests()
{
    std::vector<HttpHandle> handles;
    {
        std::lock_guard<std::mutex> lock(m_ActiveMutex);
        handles.reserve(m_Active.size());
        for (const auto& [handle, active] : m_Active)
        {
            handles.push_back(handle);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_PendingSubmits.clear();
        m_PendingCancels.insert(m_PendingCancels.end(), handles.begin(), handles.end());
    }
    // Same "cancel means no callback" rule as CancelRequest, applied to
    // everything: responses that finished but were never pumped go away too.
    {
        std::lock_guard<std::mutex> lock(m_CompletionMutex);
        m_Completions.clear();
    }
}

std::size_t NetworkSystem::GetActiveRequestCount() const
{
    std::size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        pending = m_PendingSubmits.size();
    }
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    return pending + m_Active.size();
}

std::size_t NetworkSystem::Pump()
{
    std::vector<Completion> ready;
    DrainCompletions(ready);
    if (ready.empty())
    {
        return 0;
    }
    // Callbacks run outside every lock: they are user code and may well call
    // back into SendAsync().
    for (const Completion& completion : ready)
    {
        if (completion.on_complete)
        {
            completion.on_complete(completion.response);
        }
    }
    return ready.size();
}

// =============================================================================
// Blocking HTTP
// =============================================================================

HttpResponse NetworkSystem::SendBlocking(const HttpRequestDesc& desc)
{
    HttpRequest  request;
    HttpResponse response;
    request.PerformBlocking(desc, response);
    return response;
}

HttpResponse NetworkSystem::Get(const std::string& url, const HttpRequestDesc& base_desc)
{
    HttpRequestDesc desc = base_desc;
    desc.url             = url;
    desc.method          = HttpMethod::Get;
    return SendBlocking(desc);
}

HttpResponse NetworkSystem::Post(const std::string& url,
                                 const std::string& body,
                                 const std::string& content_type)
{
    HttpRequestDesc desc;
    desc.url          = url;
    desc.method       = HttpMethod::Post;
    desc.content_type = content_type;
    desc.body.assign(body.begin(), body.end());
    return SendBlocking(desc);
}

HttpResponse NetworkSystem::PostBinary(const std::string& url,
                                       const void*        data,
                                       std::size_t        size,
                                       const std::string& content_type)
{
    HttpRequestDesc desc;
    desc.url          = url;
    desc.method       = HttpMethod::Post;
    desc.content_type = content_type;
    if (data != nullptr && size > 0)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        desc.body.assign(bytes, bytes + size);
    }
    return SendBlocking(desc);
}

HttpResponse NetworkSystem::DownloadFile(const std::string& url,
                                         const std::string& output_path,
                                         bool               resume_partial)
{
    HttpRequestDesc desc;
    desc.url            = url;
    desc.method         = HttpMethod::Get;
    desc.output_path    = output_path;
    desc.resume_partial = resume_partial;
    return SendBlocking(desc);
}

// =============================================================================
// WebSocket
// =============================================================================

std::shared_ptr<WebSocket> NetworkSystem::CreateWebSocket(const WebSocketDesc& desc)
{
    auto socket = std::make_shared<WebSocket>();
    if (!socket->Connect(desc))
    {
        LOG_WARNING(ZEngine, "NetworkSystem: WebSocket connect to '{}' failed: {}", desc.url,
                    socket->GetLastError());
    }
    std::lock_guard<std::mutex> lock(m_WebSocketMutex);
    m_WebSockets.push_back(socket);
    return socket;
}

void NetworkSystem::CloseAllWebSockets()
{
    std::vector<std::weak_ptr<WebSocket>> sockets;
    {
        std::lock_guard<std::mutex> lock(m_WebSocketMutex);
        sockets.swap(m_WebSockets);
    }
    for (const auto& weak : sockets)
    {
        if (const auto socket = weak.lock())
        {
            socket->Close();
        }
    }
}

std::size_t NetworkSystem::GetWebSocketCount() const
{
    std::lock_guard<std::mutex> lock(m_WebSocketMutex);
    std::size_t                 count = 0;
    for (const auto& weak : m_WebSockets)
    {
        if (!weak.expired())
        {
            ++count;
        }
    }
    return count;
}
