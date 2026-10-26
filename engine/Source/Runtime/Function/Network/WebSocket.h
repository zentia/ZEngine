#pragma once

/**
 * @file WebSocket.h
 * @brief RFC 6455 client on top of libcurl's curl_ws_* API.
 *
 * Threading model: every socket owns one thread. The alternative -- driving
 * many sockets from one CURLM loop -- is more scalable but forces every
 * consumer to hand its socket over to a shared reactor, and the engine has no
 * such reactor today. A game opens a handful of long-lived sockets (gateway,
 * chat, matchmaking), so one thread each is the right trade here.
 *
 * Inbound messages are queued and consumed on the caller's thread through
 * TakeMessage()/TakeMessages(); no user callback is ever invoked from the
 * socket thread, so game state is only ever touched where the game expects it.
 *
 * Same no-libcurl degradation as HttpRequest: every entry point reports
 * NetworkStatus::Unsupported on a curl-less build.
 */

#include "Runtime/Function/Network/NetworkCommon.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(ZENGINE_NETWORK_HAS_CURL)
    #include <curl/curl.h>
#endif

// =============================================================================
// Types
// =============================================================================

enum class WebSocketState : uint8_t
{
    Closed = 0,
    Connecting,
    Open,
    /// Close frame sent or received; the socket thread is winding down.
    Closing
};

inline const char* WebSocketStateToString(WebSocketState state)
{
    switch (state)
    {
        case WebSocketState::Closed:     return "Closed";
        case WebSocketState::Connecting: return "Connecting";
        case WebSocketState::Open:       return "Open";
        case WebSocketState::Closing:    return "Closing";
    }
    return "Closed";
}

enum class WebSocketMessageType : uint8_t
{
    Text = 0,
    Binary,
    /// Peer initiated the close handshake. `payload` carries the 2-byte
    /// big-endian code followed by the UTF-8 reason, when the peer sent one.
    Close,
    Ping,
    Pong
};

struct WebSocketMessage
{
    WebSocketMessageType type {WebSocketMessageType::Binary};
    std::vector<uint8_t> payload;

    std::string AsString() const
    {
        return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
    }

    /// For Close messages: the status code, or 1005 ("no status received")
    /// when the peer sent an empty body.
    uint16_t CloseCode() const
    {
        if (type != WebSocketMessageType::Close || payload.size() < 2)
        {
            return 1005;
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(payload[0]) << 8) | payload[1]);
    }

    /// For Close messages: the reason text (may be empty).
    std::string CloseReason() const
    {
        if (type != WebSocketMessageType::Close || payload.size() < 2)
        {
            return std::string();
        }
        return std::string(reinterpret_cast<const char*>(payload.data()) + 2, payload.size() - 2);
    }
};

struct WebSocketDesc
{
    /// ws:// or wss://
    std::string url;

    std::unordered_map<std::string, std::string> headers;

    /// Optional Sec-WebSocket-Protocol value (a single sub-protocol).
    std::string protocol;

    std::string ca_bundle_path;

    bool verify_peer {true};
    bool verify_host {true};

    long connect_timeout_seconds {10};

    /// Hard cap on a reassembled message; larger messages are dropped and the
    /// socket is closed with 1009.
    uint32_t max_message_bytes {8u * 1024u * 1024u};

    /// How long the socket thread parks before re-checking the close flag.
    /// Bounds shutdown latency without busy-waiting.
    uint32_t poll_interval_milliseconds {50};

    /// After we send a close frame we wait this long for the peer's close frame
    /// (RFC 6455 5.5.1). A peer that never answers costs us exactly this much
    /// shutdown latency, no more. 0 disables the wait entirely.
    uint32_t close_handshake_timeout_milliseconds {1000};

    /// Explicit proxy URL. Empty -> libcurl default (environment variables).
    std::string proxy;

    /// Force a direct connection; see HttpRequestDesc::ignore_environment_proxy.
    bool        ignore_environment_proxy {false};
};

// =============================================================================
// WebSocket
// =============================================================================

class WebSocket
{
public:
    WebSocket();
    ~WebSocket();

    WebSocket(const WebSocket&)            = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    /// Kick off the handshake on a new thread. Returns as soon as the thread is
    /// running; watch GetState() for Open, or TakeMessage() for a Close.
    bool Connect(const WebSocketDesc& desc);

    /// Graceful close: send a close frame, then stop the thread.
    void Close(uint16_t code = 1000, const std::string& reason = {});

    /// Immediate teardown: no close frame, just stop.
    void Abort();

    WebSocketState GetState() const { return m_State.load(std::memory_order_acquire); }
    bool IsOpen() const { return GetState() == WebSocketState::Open; }

    // ---- sending -----------------------------------------------------------
    bool SendText(const std::string& text);
    bool SendBinary(const void* data, std::size_t size);
    bool SendPing(const void* data = nullptr, std::size_t size = 0);

    // ---- receiving (call from the game thread) -----------------------------
    /// Pop the oldest message. False when the inbox is empty.
    bool TakeMessage(WebSocketMessage& out_message);
    /// Drain the whole inbox. Returns the number of messages moved.
    std::size_t TakeMessages(std::vector<WebSocketMessage>& out_messages);

    std::size_t GetPendingMessageCount() const;

    // ---- diagnostics -------------------------------------------------------
    // Both are written by the socket thread and read from the game thread, so
    // they go through m_StatusMutex rather than being plain members.
    NetworkStatus GetLastStatus() const;
    std::string   GetLastError() const;

    /// HTTP status reported by the server during the WS upgrade (101 on
    /// success). 0 when the handshake never got a response.
    int GetUpgradeStatus() const { return m_UpgradeStatus.load(std::memory_order_acquire); }

private:
    struct OutgoingMessage
    {
        std::vector<uint8_t> payload;
        bool                 is_close {false};
        bool                 is_ping {false};
        bool                 is_text {false};
    };

    void ThreadMain();
    void SetState(WebSocketState state);
    void SetFailure(NetworkStatus status, const std::string& message);

    bool PumpOutgoing();
    bool PumpIncoming();

    /// Wait (bounded) for the peer's close frame after we sent ours, so the
    /// game thread can observe the final Close message.
    void DrainCloseReply(int interval_milliseconds);

    void PushIncoming(WebSocketMessage&& message);
    bool PopOutgoing(OutgoingMessage& out_message);

#if defined(ZENGINE_NETWORK_HAS_CURL)
    CURL*       m_Curl {nullptr};
    curl_slist* m_HeaderList {nullptr};
    char        m_ErrorBuffer[CURL_ERROR_SIZE] {'\0'};
#endif

    WebSocketDesc m_Desc;

    std::thread m_Thread;

    std::atomic<WebSocketState> m_State {WebSocketState::Closed};
    /// Abort(): tear the thread down now, no close frame.
    std::atomic<bool>           m_StopRequested {false};
    /// Close(): flush the queue (which carries the close frame), then exit.
    std::atomic<bool>           m_CloseRequested {false};
    /// Set once our close frame is on the wire. The loop exits on this, not on
    /// m_CloseRequested, so a Close() racing with an empty-queue pump still
    /// gets its frame sent next round instead of being dropped.
    std::atomic<bool>           m_CloseFrameSent {false};
    std::atomic<int>            m_UpgradeStatus {0};

    /// Partially reassembled user message. Touched only by the socket thread,
    /// so it needs no lock.
    std::vector<uint8_t>        m_Reassembly;

    mutable std::mutex      m_OutgoingMutex;
    std::deque<OutgoingMessage> m_Outgoing;

    mutable std::mutex            m_IncomingMutex;
    std::deque<WebSocketMessage>  m_Incoming;

    mutable std::mutex m_StatusMutex;
    NetworkStatus      m_LastStatus {NetworkStatus::Success};
    std::string        m_LastError;
};
