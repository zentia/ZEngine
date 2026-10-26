#pragma once

/**
 * @file NetworkSystem.h
 * @brief Engine-facing owner of the network transports.
 *
 * Responsibilities, kept deliberately narrow:
 *
 *   1. libcurl process-wide init/cleanup (curl_global_init is not thread-safe
 *      and must happen exactly once -- parking it here stops every caller from
 *      having to remember).
 *   2. One worker thread driving a CURLM, so HTTP never stalls the game thread.
 *      Completions are queued and delivered from Pump(), which the caller
 *      invokes once per frame: user callbacks therefore always run on the
 *      caller's thread, never on the worker.
 *   3. Lifetime bookkeeping for WebSockets (so Shutdown() cannot leave a
 *      socket thread running past the last frame).
 *
 * UDP needs none of this -- UdpSocket is self-contained (it calls WSAStartup
 * itself on Windows) and is instantiated directly by whichever layer owns the
 * netcode loop.
 *
 * Why Pump() rather than a Tick(): IEngineSystem has no per-frame callback, and
 * ModuleManager::TickAllModules() is currently never called by the main loop.
 * An explicit Pump() keeps the delivery point visible instead of hiding it in
 * a registration nobody wires up.
 */

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Network/HttpRequest.h"
#include "Runtime/Function/Network/NetworkCommon.h"
#include "Runtime/Function/Network/UdpSocket.h"
#include "Runtime/Function/Network/WebSocket.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class NetworkSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "NetworkSystem"; }
    /// No dependencies on other systems: register early so gameplay code can
    /// fire requests during its own Initialize().
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }

    /// False when the transport could not be started (curl-less build, or
    /// curl_global_init/curl_multi_init failed). Requests then complete with
    /// NetworkStatus::Unsupported instead of crashing or hanging.
    ///
    /// A missing transport must never fail engine startup: a game that has no
    /// networking still boots. This mirrors AudioSystem, which degrades to the
    /// null device instead of returning false from Initialize().
    bool IsBackendAvailable() const { return m_BackendReady.load(std::memory_order_acquire); }

    // =========================================================================
    // Asynchronous HTTP (worker thread)
    // =========================================================================

    /// Queue a request. The callback runs on the thread that calls Pump().
    /// Returns kInvalidHttpHandle when the descriptor is unusable or the
    /// system is not initialized.
    HttpHandle SendAsync(const HttpRequestDesc& desc, HttpCompletionCallback on_complete = {});

    /// True once the request has finished (or was cancelled, or never existed).
    bool IsRequestDone(HttpHandle handle) const;

    /// Byte counters for a running request. Zeroed for unknown handles.
    HttpProgress GetRequestProgress(HttpHandle handle) const;

    /// Stop one request. The completion callback is dropped, not invoked.
    bool CancelRequest(HttpHandle handle);

    void CancelAllRequests();

    std::size_t GetActiveRequestCount() const;

    /// Deliver finished responses. Call once per frame from the game thread.
    /// Returns the number of callbacks invoked.
    std::size_t Pump();

    // =========================================================================
    // Blocking HTTP (calling thread)
    //
    // For editor tooling, cook steps and tests. Never call these from the game
    // thread at runtime -- they block for the whole transfer.
    // =========================================================================

    HttpResponse SendBlocking(const HttpRequestDesc& desc);
    HttpResponse Get(const std::string& url, const HttpRequestDesc& base_desc = {});
    HttpResponse Post(const std::string& url,
                      const std::string& body,
                      const std::string& content_type = "application/json");
    HttpResponse PostBinary(const std::string& url,
                            const void*       data,
                            std::size_t       size,
                            const std::string& content_type = "application/octet-stream");
    /// Stream a URL straight to disk. Returns the response with an empty body.
    HttpResponse DownloadFile(const std::string& url,
                              const std::string& output_path,
                              bool               resume_partial = false);

    // =========================================================================
    // WebSocket
    // =========================================================================

    /// Create + connect. The socket is tracked so Shutdown() can close it;
    /// dropping the returned shared_ptr also closes it.
    std::shared_ptr<WebSocket> CreateWebSocket(const WebSocketDesc& desc);

    void CloseAllWebSockets();
    std::size_t GetWebSocketCount() const;

    // =========================================================================
    // UDP
    // =========================================================================

    /// Convenience factory so UDP sockets are created the same way everywhere;
    /// UdpSocket has no other dependency on this system.
    std::shared_ptr<UdpSocket> CreateUdpSocket() { return std::make_shared<UdpSocket>(); }

protected:
    bool Initialize() override;
    void Shutdown() override;

private:
    struct ActiveRequest
    {
        std::shared_ptr<HttpRequest> request;
        HttpCompletionCallback       on_complete;
    };

    struct Completion
    {
        /// Carried so CancelRequest() can drop an already-finished response
        /// that has not been pumped yet: without it, a request that completed
        /// before the cancel reached the worker would still invoke its
        /// callback, and "cancel means no callback" would be a race rather
        /// than a contract.
        HttpHandle             handle {kInvalidHttpHandle};
        HttpResponse           response;
        HttpCompletionCallback on_complete;
    };

    /// A request accepted by SendAsync() but not yet handed to the CURLM.
    /// Additions happen on the worker thread because curl_multi_add_handle()
    /// must not race with curl_multi_perform().
    struct PendingSubmit
    {
        HttpHandle    handle {kInvalidHttpHandle};
        ActiveRequest request;
    };

    void WorkerMain();
    void DrainSubmits();
    void DrainCancels();
    void DrainCompletions(std::vector<Completion>& out_ready);
    bool IsHandlePending(HttpHandle handle) const;

#if defined(ZENGINE_NETWORK_HAS_CURL)
    CURLM* m_Multi {nullptr};
#endif

    std::thread m_Worker;
    std::atomic<bool> m_StopRequested {false};
    std::atomic<bool> m_Initialized {false};
    /// Set only once the transport is actually usable (worker running, or -- on
    /// a curl-less build -- the no-backend path taken deliberately). Distinct
    /// from m_Initialized: the system always initializes, the transport may not.
    std::atomic<bool> m_BackendReady {false};

    std::atomic<HttpHandle> m_NextHandle {1};

    // Guards m_PendingSubmits and m_PendingCancels. Never taken while
    // m_ActiveMutex is held (and vice versa): both are read by the game thread
    // and written by the worker, and nesting them would make the order matter.
    mutable std::mutex              m_SubmitMutex;
    std::vector<PendingSubmit>      m_PendingSubmits;
    std::vector<HttpHandle>         m_PendingCancels;

    // Guards both m_Active and m_HandleByCurl: written by the worker, read by
    // IsRequestDone()/GetRequestProgress() from the game thread.
    mutable std::mutex                        m_ActiveMutex;
    std::unordered_map<HttpHandle, ActiveRequest> m_Active;
#if defined(ZENGINE_NETWORK_HAS_CURL)
    std::unordered_map<CURL*, HttpHandle>     m_HandleByCurl;
#endif

    mutable std::mutex         m_CompletionMutex;
    std::vector<Completion>    m_Completions;

    mutable std::mutex                   m_WebSocketMutex;
    std::vector<std::weak_ptr<WebSocket>> m_WebSockets;
};
