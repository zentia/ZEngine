#pragma once

/**
 * @file UdpSocket.h
 * @brief Connectionless UDP socket, thin cross-platform wrapper.
 *
 * This is the transport a netcode layer builds on: no handshake, no
 * retransmission, no ordering -- those belong to a higher layer. The class is
 * deliberately a plain RAII wrapper over the platform socket so that the
 * higher layer can own the threading model.
 *
 * The platform headers (<winsock2.h> / <sys/socket.h>) are confined to the
 * .cpp; the header stores the handle as an integer so that including this file
 * cannot leak windows.h macros into the rest of the engine.
 *
 * Receive convention (mirrors what a game loop wants):
 *   >0  bytes received, `out_from` filled
 *    0  nothing available right now (non-blocking socket), not an error
 *   -1  hard error; see GetLastStatus() / GetLastError()
 */

#include "Runtime/Function/Network/NetworkCommon.h"

#include <cstdint>
#include <string>

/// Raw socket descriptor as an integer. -1 on every platform
/// (INVALID_SOCKET is (SOCKET)~0, which is -1 in two's complement).
using UdpSocketHandleValue = std::int64_t;
inline constexpr UdpSocketHandleValue kInvalidUdpSocketHandle = -1;

class UdpSocket
{
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // ---- lifetime ---------------------------------------------------------
    /// Bind to `local_port` on all interfaces. Pass 0 to let the OS pick a
    /// port (then read it back with GetLocalPort()).
    bool Open(uint16_t local_port = 0, bool reuse_address = true);

    /// Bind to one specific interface, e.g. "127.0.0.1" or "::1".
    bool OpenBound(const std::string& local_address, uint16_t local_port, bool reuse_address = true);

    void Close();
    bool IsOpen() const { return m_Handle != kInvalidUdpSocketHandle; }

    uint16_t GetLocalPort() const;

    // ---- configuration ----------------------------------------------------
    /// Almost always what you want for a game loop: ReceiveFrom() then returns
    /// 0 instead of blocking.
    bool SetNonBlocking(bool enabled);

    /// Required before SendTo() on a broadcast address (255.255.255.255 or a
    /// subnet broadcast) -- the default is off.
    bool SetBroadcast(bool enabled);

    bool SetSendBufferSize(int bytes);
    bool SetReceiveBufferSize(int bytes);

    /// Join an IPv4/IPv6 multicast group. `local_interface` empty -> OS picks.
    bool JoinMulticastGroup(const std::string& group_address, const std::string& local_interface = {});
    bool LeaveMulticastGroup(const std::string& group_address, const std::string& local_interface = {});
    bool SetMulticastTtl(int hops);

    // ---- I/O --------------------------------------------------------------
    bool SendTo(const NetworkEndpoint& destination, const void* data, std::size_t size);

    /// Receive one datagram. Returns bytes, 0 for "would block", -1 on error.
    /// Datagrams larger than `capacity` are truncated, as with recvfrom().
    int ReceiveFrom(void* buffer, std::size_t capacity, NetworkEndpoint& out_from);

    // ---- diagnostics ------------------------------------------------------
    NetworkStatus     GetLastStatus() const { return m_LastStatus; }
    const std::string& GetLastError() const { return m_LastError; }

private:
    bool EnsureOpen() const;
    void SetFailure(NetworkStatus status, const std::string& message);

    UdpSocketHandleValue m_Handle {kInvalidUdpSocketHandle};
    NetworkStatus        m_LastStatus {NetworkStatus::Success};
    std::string          m_LastError;
};
