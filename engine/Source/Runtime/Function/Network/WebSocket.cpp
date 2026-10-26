#include "Runtime/Function/Network/WebSocket.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Network/HttpRequest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(ZENGINE_NETWORK_HAS_CURL)
    #include <curl/curl.h>
    #if defined(_WIN32)
        #ifndef WIN32_LEAN_AND_MEAN
            #define WIN32_LEAN_AND_MEAN
        #endif
        #include <winsock2.h>
        #include <ws2tcpip.h>
    #else
        #include <poll.h>
    #endif
#endif

// Everything below is only reachable when libcurl is present: the whole file
// degrades to "no WebSocket support" without it, mirroring HttpRequest.
#if defined(ZENGINE_NETWORK_HAS_CURL)

namespace
{
    /// Block the calling (socket) thread until the socket has something to read
    /// or `timeout_ms` elapses.
    ///
    /// Returns >0 when readable, 0 on timeout, <0 on error.
    int WebSocketWaitReadable(int socket_fd, int timeout_ms)
    {
        if (socket_fd < 0)
        {
            return -1;
        }
#if defined(_WIN32)
        WSAPOLLFD entry {};
        entry.fd     = static_cast<SOCKET>(socket_fd);
        entry.events = POLLIN;
        return WSAPoll(&entry, 1, timeout_ms);
#else
        pollfd entry {};
        entry.fd     = socket_fd;
        entry.events = POLLIN;
        return ::poll(&entry, 1, timeout_ms);
#endif
    }

    constexpr uint16_t kCloseMessageTooBig = 1009;
}  // namespace

#endif  // ZENGINE_NETWORK_HAS_CURL

// =============================================================================
// Lifetime
// =============================================================================

WebSocket::WebSocket() = default;

WebSocket::~WebSocket()
{
    Abort();
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }
#if defined(ZENGINE_NETWORK_HAS_CURL)
    if (m_HeaderList != nullptr)
    {
        curl_slist_free_all(m_HeaderList);
        m_HeaderList = nullptr;
    }
#endif
}

bool WebSocket::Connect(const WebSocketDesc& desc)
{
    if (GetState() != WebSocketState::Closed)
    {
        return false;
    }
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }

    m_Desc = desc;

    {
        std::lock_guard<std::mutex> lock(m_StatusMutex);
        m_LastStatus = NetworkStatus::Success;
        m_LastError.clear();
    }

#if !defined(ZENGINE_NETWORK_HAS_CURL)
    SetFailure(NetworkStatus::Unsupported, "WebSocket is unavailable: this build has no libcurl");
    return false;
#else
    if (m_Desc.url.empty())
    {
        SetFailure(NetworkStatus::InvalidArgument, "empty URL");
        return false;
    }

    m_Curl = curl_easy_init();
    if (m_Curl == nullptr)
    {
        SetFailure(NetworkStatus::NotInitialized,
                   "curl_easy_init() failed; is curl_global_init() done?");
        return false;
    }

    std::memset(m_ErrorBuffer, 0, sizeof(m_ErrorBuffer));

    curl_easy_setopt(m_Curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_Curl, CURLOPT_ERRORBUFFER, m_ErrorBuffer);
    curl_easy_setopt(m_Curl, CURLOPT_URL, m_Desc.url.c_str());
    // 2L == "connect, then perform the WebSocket upgrade and hand control
    // back" -- the only mode in which curl_ws_send()/curl_ws_recv() work.
    curl_easy_setopt(m_Curl, CURLOPT_CONNECT_ONLY, 2L);

    if (!m_Desc.protocol.empty())
    {
        m_HeaderList = curl_slist_append(
            m_HeaderList, ("Sec-WebSocket-Protocol: " + m_Desc.protocol).c_str());
    }
    for (const auto& [name, value] : m_Desc.headers)
    {
        if (name.empty())
        {
            continue;
        }
        m_HeaderList = curl_slist_append(m_HeaderList, (name + ": " + value).c_str());
    }
    if (m_HeaderList != nullptr)
    {
        curl_easy_setopt(m_Curl, CURLOPT_HTTPHEADER, m_HeaderList);
    }

    curl_easy_setopt(m_Curl, CURLOPT_SSL_VERIFYPEER, m_Desc.verify_peer ? 1L : 0L);
    curl_easy_setopt(m_Curl, CURLOPT_SSL_VERIFYHOST, m_Desc.verify_host ? 2L : 0L);
    if (!m_Desc.ca_bundle_path.empty())
    {
        curl_easy_setopt(m_Curl, CURLOPT_CAINFO, m_Desc.ca_bundle_path.c_str());
    }
    if (m_Desc.connect_timeout_seconds > 0)
    {
        curl_easy_setopt(m_Curl, CURLOPT_CONNECTTIMEOUT, m_Desc.connect_timeout_seconds);
    }
    if (m_Desc.ignore_environment_proxy)
    {
        curl_easy_setopt(m_Curl, CURLOPT_PROXY, "");
    }
    else if (!m_Desc.proxy.empty())
    {
        curl_easy_setopt(m_Curl, CURLOPT_PROXY, m_Desc.proxy.c_str());
    }

    m_StopRequested.store(false, std::memory_order_release);
    m_CloseRequested.store(false, std::memory_order_release);
    m_UpgradeStatus.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_IncomingMutex);
        m_Incoming.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_OutgoingMutex);
        m_Outgoing.clear();
    }
    m_Reassembly.clear();

    SetState(WebSocketState::Connecting);
    m_Thread = std::thread(&WebSocket::ThreadMain, this);
    return true;
#endif
}

void WebSocket::Close(uint16_t code, const std::string& reason)
{
    if (GetState() == WebSocketState::Closed)
    {
        return;
    }

    OutgoingMessage closing;
    closing.is_close = true;
    closing.payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    closing.payload.push_back(static_cast<uint8_t>(code & 0xFF));
    closing.payload.insert(closing.payload.end(), reason.begin(), reason.end());

    {
        std::lock_guard<std::mutex> lock(m_OutgoingMutex);
        m_Outgoing.push_back(std::move(closing));
    }
    // The socket thread flushes the queue, sees the flag and exits. We do NOT
    // set m_StopRequested: that would kill the thread before the close frame
    // reaches the peer.
    m_CloseRequested.store(true, std::memory_order_release);
}

void WebSocket::Abort()
{
    m_StopRequested.store(true, std::memory_order_release);
}

// =============================================================================
// Sending
// =============================================================================

bool WebSocket::SendText(const std::string& text)
{
    OutgoingMessage message;
    message.is_text = true;
    message.payload.assign(text.begin(), text.end());
    {
        std::lock_guard<std::mutex> lock(m_OutgoingMutex);
        m_Outgoing.push_back(std::move(message));
    }
    return true;
}

bool WebSocket::SendBinary(const void* data, std::size_t size)
{
    if (data == nullptr && size > 0)
    {
        return false;
    }
    OutgoingMessage message;
    const auto* bytes = static_cast<const uint8_t*>(data);
    message.payload.assign(bytes, bytes + size);
    {
        std::lock_guard<std::mutex> lock(m_OutgoingMutex);
        m_Outgoing.push_back(std::move(message));
    }
    return true;
}

bool WebSocket::SendPing(const void* data, std::size_t size)
{
    OutgoingMessage message;
    message.is_ping = true;
    if (data != nullptr && size > 0)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        message.payload.assign(bytes, bytes + size);
    }
    {
        std::lock_guard<std::mutex> lock(m_OutgoingMutex);
        m_Outgoing.push_back(std::move(message));
    }
    return true;
}

// =============================================================================
// Receiving
// =============================================================================

bool WebSocket::TakeMessage(WebSocketMessage& out_message)
{
    std::lock_guard<std::mutex> lock(m_IncomingMutex);
    if (m_Incoming.empty())
    {
        return false;
    }
    out_message = std::move(m_Incoming.front());
    m_Incoming.pop_front();
    return true;
}

std::size_t WebSocket::TakeMessages(std::vector<WebSocketMessage>& out_messages)
{
    std::lock_guard<std::mutex> lock(m_IncomingMutex);
    const std::size_t           count = m_Incoming.size();
    out_messages.reserve(out_messages.size() + count);
    while (!m_Incoming.empty())
    {
        out_messages.push_back(std::move(m_Incoming.front()));
        m_Incoming.pop_front();
    }
    return count;
}

std::size_t WebSocket::GetPendingMessageCount() const
{
    std::lock_guard<std::mutex> lock(m_IncomingMutex);
    return m_Incoming.size();
}

// =============================================================================
// Diagnostics
// =============================================================================

NetworkStatus WebSocket::GetLastStatus() const
{
    std::lock_guard<std::mutex> lock(m_StatusMutex);
    return m_LastStatus;
}

std::string WebSocket::GetLastError() const
{
    std::lock_guard<std::mutex> lock(m_StatusMutex);
    return m_LastError;
}

void WebSocket::SetState(WebSocketState state)
{
    m_State.store(state, std::memory_order_release);
}

void WebSocket::SetFailure(NetworkStatus status, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_StatusMutex);
    m_LastStatus = status;
    m_LastError  = message;
}

// =============================================================================
// Socket thread
// =============================================================================

void WebSocket::ThreadMain()
{
#if defined(ZENGINE_NETWORK_HAS_CURL)
    const CURLcode handshake = curl_easy_perform(m_Curl);
    if (handshake != CURLE_OK)
    {
        const NetworkStatus status = HttpRequest::TranslateCurlResult(static_cast<int>(handshake));
        const std::string   detail =
            m_ErrorBuffer[0] != '\0' ? std::string(m_ErrorBuffer) : curl_easy_strerror(handshake);
        LOG_ERROR(ZEngine, "WebSocket handshake with '{}' failed: {}", m_Desc.url, detail);
        SetFailure(status, detail);
        SetState(WebSocketState::Closed);
        return;
    }

    long upgrade_status = 0;
    curl_easy_getinfo(m_Curl, CURLINFO_RESPONSE_CODE, &upgrade_status);
    m_UpgradeStatus.store(static_cast<int>(upgrade_status), std::memory_order_release);

    if (upgrade_status != 101)
    {
        const std::string detail = "server answered the upgrade with HTTP " +
                                   std::to_string(upgrade_status) + ", expected 101";
        LOG_ERROR(ZEngine, "WebSocket upgrade of '{}' rejected: {}", m_Desc.url, detail);
        SetFailure(NetworkStatus::ConnectFailure, detail);
        SetState(WebSocketState::Closed);
        return;
    }

    LOG_INFO(ZEngine, "WebSocket connected: {}", m_Desc.url);
    SetState(WebSocketState::Open);

    const int interval = static_cast<int>(m_Desc.poll_interval_milliseconds);

    while (!m_StopRequested.load(std::memory_order_acquire))
    {
        if (!PumpOutgoing())
        {
            break;  // close frame sent, or a fatal send error
        }
        if (m_CloseFrameSent.load(std::memory_order_acquire))
        {
            break;
        }

        curl_socket_t active_socket = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(m_Curl, CURLINFO_ACTIVESOCKET, &active_socket) == CURLE_OK)
        {
            const int ready = WebSocketWaitReadable(static_cast<int>(active_socket), interval);
            if (ready > 0)
            {
                if (!PumpIncoming())
                {
                    break;
                }
            }
            else if (ready < 0)
            {
                SetFailure(NetworkStatus::IoFailure, "socket poll failed");
                break;
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }

    // RFC 6455 5.5.1: having sent a close frame we are supposed to wait for the
    // peer's close frame before dropping the connection. Skipping that wait is
    // the common shortcut, but it hides the peer's final status code/reason
    // from the game. So we linger for a bounded grace period: long enough for
    // a well-behaved server to answer, short enough that a silent one cannot
    // hold the thread (and therefore Shutdown()) hostage.
    if (m_CloseFrameSent.load(std::memory_order_acquire) &&
        !m_StopRequested.load(std::memory_order_acquire))
    {
        DrainCloseReply(interval);
    }

    m_Reassembly.clear();

    if (m_Curl != nullptr)
    {
        curl_easy_cleanup(m_Curl);
        m_Curl = nullptr;
    }
    SetState(WebSocketState::Closed);
#endif
}

void WebSocket::DrainCloseReply(int interval_milliseconds)
{
#if defined(ZENGINE_NETWORK_HAS_CURL)
    const uint32_t grace = m_Desc.close_handshake_timeout_milliseconds;
    if (grace == 0)
    {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace);

    while (!m_StopRequested.load(std::memory_order_acquire))
    {
        const auto now      = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int  wait      = static_cast<int>(std::min<std::int64_t>(remaining.count(),
                                                                      std::max(1, interval_milliseconds)));

        curl_socket_t active_socket = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(m_Curl, CURLINFO_ACTIVESOCKET, &active_socket) != CURLE_OK ||
            active_socket == CURL_SOCKET_BAD)
        {
            break;
        }

        const int ready = WebSocketWaitReadable(static_cast<int>(active_socket), wait);
        if (ready < 0)
        {
            break;
        }
        if (ready == 0)
        {
            continue;  // nothing yet; loop re-checks the deadline
        }
        if (!PumpIncoming())
        {
            break;  // peer's close frame (or a fatal receive error)
        }
    }
#endif
}

bool WebSocket::PumpOutgoing()
{
#if !defined(ZENGINE_NETWORK_HAS_CURL)
    return false;
#else
    for (;;)
    {
        OutgoingMessage message;
        if (!PopOutgoing(message))
        {
            return true;
        }

        unsigned int flags = CURLWS_BINARY;
        if (message.is_close)
        {
            flags = CURLWS_CLOSE;
        }
        else if (message.is_ping)
        {
            flags = CURLWS_PING;
        }
        else if (message.is_text)
        {
            flags = CURLWS_TEXT;
        }

        const void* payload = message.payload.empty()
                                  ? static_cast<const void*>("")
                                  : static_cast<const void*>(message.payload.data());

        std::size_t sent   = 0;
        const CURLcode code = curl_ws_send(m_Curl, payload, message.payload.size(), &sent, 0, flags);

        if (code == CURLE_AGAIN)
        {
            // Socket buffer is full; keep the message at the front and retry on
            // the next iteration.
            std::lock_guard<std::mutex> lock(m_OutgoingMutex);
            m_Outgoing.push_front(std::move(message));
            return true;
        }
        if (code != CURLE_OK)
        {
            const NetworkStatus status = HttpRequest::TranslateCurlResult(static_cast<int>(code));
            const std::string   detail = curl_easy_strerror(code);
            LOG_ERROR(ZEngine, "WebSocket send failed: {}", detail);
            SetFailure(status, detail);
            return false;
        }

        if (message.is_close)
        {
            SetState(WebSocketState::Closing);
            m_CloseFrameSent.store(true, std::memory_order_release);
            return false;
        }
    }
#endif
}

bool WebSocket::PumpIncoming()
{
#if !defined(ZENGINE_NETWORK_HAS_CURL)
    return false;
#else
    // One iteration per curl_ws_recv() chunk. `frame` holds the frame currently
    // being read; `m_Reassembly` holds the user message it may be a fragment
    // of. Keeping them separate is what makes a control frame arriving between
    // two fragments of a text/binary message harmless -- the control frame is
    // emitted on its own and never mixed into the user payload.
    std::vector<uint8_t> frame;

    for (;;)
    {
        uint8_t                  buffer[4096];
        std::size_t              received = 0;
        const curl_ws_frame*     meta     = nullptr;

        const CURLcode code = curl_ws_recv(m_Curl, buffer, sizeof(buffer), &received, &meta);
        if (code == CURLE_AGAIN)
        {
            return true;  // drained for now
        }
        if (code != CURLE_OK)
        {
            const NetworkStatus status = HttpRequest::TranslateCurlResult(static_cast<int>(code));
            const std::string   detail = curl_easy_strerror(code);
            LOG_ERROR(ZEngine, "WebSocket receive failed: {}", detail);
            SetFailure(status, detail);
            m_Reassembly.clear();
            return false;
        }
        if (meta == nullptr)
        {
            return true;
        }

        frame.insert(frame.end(), buffer, buffer + received);

        if (frame.size() > m_Desc.max_message_bytes)
        {
            m_Reassembly.clear();
            frame.clear();
            SetFailure(NetworkStatus::TooLarge,
                       "incoming message exceeded " + std::to_string(m_Desc.max_message_bytes) +
                           " bytes");
            // 1009 is the RFC 6455 code for exactly this.
            Close(kCloseMessageTooBig, "message too big");
            return false;
        }

        // More bytes of *this frame* are still pending.
        if (meta->bytesleft > 0)
        {
            continue;
        }

        const bool is_control = (meta->flags & (CURLWS_CLOSE | CURLWS_PING | CURLWS_PONG)) != 0;
        if (is_control)
        {
            WebSocketMessage message;
            if (meta->flags & CURLWS_CLOSE)
            {
                message.type = WebSocketMessageType::Close;
            }
            else if (meta->flags & CURLWS_PING)
            {
                message.type = WebSocketMessageType::Ping;
            }
            else
            {
                message.type = WebSocketMessageType::Pong;
            }
            message.payload = std::move(frame);
            frame.clear();
            PushIncoming(std::move(message));

            if (meta->flags & CURLWS_CLOSE)
            {
                LOG_INFO(ZEngine, "WebSocket closed by peer: {}", m_Desc.url);
                m_Reassembly.clear();
                return false;
            }
            continue;
        }

        m_Reassembly.insert(m_Reassembly.end(), frame.begin(), frame.end());
        frame.clear();

        // CURLWS_CONT marks every fragment except the last one.
        if (meta->flags & CURLWS_CONT)
        {
            continue;
        }

        WebSocketMessage message;
        message.type    = (meta->flags & CURLWS_TEXT) != 0 ? WebSocketMessageType::Text
                                                           : WebSocketMessageType::Binary;
        message.payload = std::move(m_Reassembly);
        m_Reassembly.clear();
        PushIncoming(std::move(message));
    }
#endif
}

void WebSocket::PushIncoming(WebSocketMessage&& message)
{
    std::lock_guard<std::mutex> lock(m_IncomingMutex);
    m_Incoming.push_back(std::move(message));
}

bool WebSocket::PopOutgoing(OutgoingMessage& out_message)
{
    std::lock_guard<std::mutex> lock(m_OutgoingMutex);
    if (m_Outgoing.empty())
    {
        return false;
    }
    out_message = std::move(m_Outgoing.front());
    m_Outgoing.pop_front();
    return true;
}
