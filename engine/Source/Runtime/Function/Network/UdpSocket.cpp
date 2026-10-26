#include "Runtime/Function/Network/UdpSocket.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <net/if.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

#include <cstring>
#include <mutex>
#include <string>

// =============================================================================
// Platform shims
// =============================================================================

#if defined(_WIN32)
namespace
{
    /// WSAStartup is process-global and must precede any socket call. Doing it
    /// from a call_once inside the transport (rather than from
    /// NetworkSystem::Initialize) keeps UdpSocket usable from standalone tools
    /// and smoke tests that never start the engine.
    void EnsureWinsock()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            WSADATA data {};
            (void)WSAStartup(MAKEWORD(2, 2), &data);
        });
    }

    using SocketHandle = SOCKET;
    using SocketSsize  = int;

    constexpr SocketHandle kInvalidPlatformSocket = INVALID_SOCKET;

    SocketHandle ToHandle(UdpSocketHandleValue value) { return static_cast<SOCKET>(value); }
    UdpSocketHandleValue FromHandle(SocketHandle handle)
    {
        return handle == INVALID_SOCKET ? kInvalidUdpSocketHandle
                                        : static_cast<UdpSocketHandleValue>(handle);
    }

    int LastSocketError() { return static_cast<int>(WSAGetLastError()); }

    std::string SocketErrorText(int code) { return "WSA error " + std::to_string(code); }

    bool WouldBlock(int code) { return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS; }

    /// Windows reports the ICMP "port unreachable" triggered by an earlier
    /// send as a failed recvfrom(); for UDP that is normal traffic, not an
    /// error, so it is folded into "nothing to read".
    bool SoftRecvError(int code) { return code == WSAECONNRESET || code == WSAECONNREFUSED; }

    /// Windows exposes no if_nametoindex() in the winsock headers, so an
    /// interface name is accepted as its numeric index instead.
    unsigned int MulticastInterfaceIndex(const std::string& name)
    {
        if (name.empty())
        {
            return 0;
        }
        return static_cast<unsigned int>(std::strtoul(name.c_str(), nullptr, 10));
    }

    constexpr int kSendFlags = 0;
}  // namespace
#else
namespace
{
    void EnsureWinsock() {}

    using SocketHandle = int;
    using SocketSsize  = ssize_t;

    constexpr SocketHandle kInvalidPlatformSocket = -1;

    SocketHandle ToHandle(UdpSocketHandleValue value) { return static_cast<int>(value); }
    UdpSocketHandleValue FromHandle(SocketHandle handle)
    {
        return handle < 0 ? kInvalidUdpSocketHandle : static_cast<UdpSocketHandleValue>(handle);
    }

    int LastSocketError() { return errno; }

    std::string SocketErrorText(int code)
    {
        const char* text = std::strerror(code);
        return text != nullptr ? std::string(text) : std::to_string(code);
    }

    bool WouldBlock(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS; }

    bool SoftRecvError(int code) { return code == ECONNREFUSED || code == EINTR; }

    unsigned int MulticastInterfaceIndex(const std::string& name)
    {
        return name.empty() ? 0u : if_nametoindex(name.c_str());
    }

    // MSG_NOSIGNAL does not exist on macOS/BSD; UDP never raises SIGPIPE for
    // a datagram send anyway, so plain 0 flags is correct there.
    #if defined(MSG_NOSIGNAL)
    constexpr int kSendFlags = MSG_NOSIGNAL;
    #else
    constexpr int kSendFlags = 0;
    #endif
}  // namespace
#endif

namespace
{
    /// Fill a sockaddr_storage from an endpoint.
    ///
    /// inet_pton() first: game traffic is overwhelmingly literal IPs and it
    /// never blocks. getaddrinfo() is only the fallback for hostnames, and is
    /// marked as blocking in the caller's contract.
    bool EndpointToSockaddr(const NetworkEndpoint& endpoint,
                            sockaddr_storage&      out_address,
                            socklen_t&             out_length)
    {
        std::memset(&out_address, 0, sizeof(out_address));

        const std::string& host = endpoint.address;
        const uint16_t     port = endpoint.port;

        in_addr  ipv4 {};
        in6_addr ipv6 {};

        if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1)
        {
            auto* address        = reinterpret_cast<sockaddr_in*>(&out_address);
            address->sin_family  = AF_INET;
            address->sin_port    = htons(port);
            address->sin_addr    = ipv4;
            out_length           = sizeof(sockaddr_in);
            return true;
        }
        if (inet_pton(AF_INET6, host.c_str(), &ipv6) == 1)
        {
            auto* address       = reinterpret_cast<sockaddr_in6*>(&out_address);
            address->sin6_family = AF_INET6;
            address->sin6_port   = htons(port);
            address->sin6_addr   = ipv6;
            out_length           = sizeof(sockaddr_in6);
            return true;
        }

        addrinfo hints {};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* resolved = nullptr;
        const int status   = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved);
        if (status != 0 || resolved == nullptr)
        {
            return false;
        }
        const size_t copy_size = resolved->ai_addrlen < sizeof(out_address)
                                     ? static_cast<size_t>(resolved->ai_addrlen)
                                     : sizeof(out_address);
        std::memcpy(&out_address, resolved->ai_addr, copy_size);
        out_length = static_cast<socklen_t>(copy_size);
        freeaddrinfo(resolved);
        return true;
    }

    bool SockaddrToEndpoint(const sockaddr_storage& address, NetworkEndpoint& out_endpoint)
    {
        char host[INET6_ADDRSTRLEN + 1] {};
        if (address.ss_family == AF_INET)
        {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
            if (inet_ntop(AF_INET, &ipv4->sin_addr, host, sizeof(host)) == nullptr)
            {
                return false;
            }
            out_endpoint.address = host;
            out_endpoint.port    = ntohs(ipv4->sin_port);
            return true;
        }
        if (address.ss_family == AF_INET6)
        {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
            if (inet_ntop(AF_INET6, &ipv6->sin6_addr, host, sizeof(host)) == nullptr)
            {
                return false;
            }
            out_endpoint.address = host;
            out_endpoint.port    = ntohs(ipv6->sin6_port);
            return true;
        }
        return false;
    }

    /// Convert a textual address into raw bytes + family, for the multicast
    /// ioctls. False when the text is not a literal IP.
    bool ParseLiteralAddress(const std::string& text, int& out_family, in_addr& out_ipv4, in6_addr& out_ipv6)
    {
        if (inet_pton(AF_INET, text.c_str(), &out_ipv4) == 1)
        {
            out_family = AF_INET;
            return true;
        }
        if (inet_pton(AF_INET6, text.c_str(), &out_ipv6) == 1)
        {
            out_family = AF_INET6;
            return true;
        }
        return false;
    }
}  // namespace

// =============================================================================
// Lifetime
// =============================================================================

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::Open(uint16_t local_port, bool reuse_address)
{
    return OpenBound(std::string(), local_port, reuse_address);
}

bool UdpSocket::OpenBound(const std::string& local_address, uint16_t local_port, bool reuse_address)
{
    Close();
    EnsureWinsock();

    sockaddr_storage bind_address {};
    socklen_t        bind_length = 0;

    if (!local_address.empty())
    {
        NetworkEndpoint endpoint;
        endpoint.address = local_address;
        endpoint.port    = local_port;
        if (!EndpointToSockaddr(endpoint, bind_address, bind_length))
        {
            SetFailure(NetworkStatus::InvalidArgument, "cannot resolve bind address '" + local_address + "'");
            return false;
        }
    }
    else
    {
        // Wildcard bind. AF_INET6 with IPV6_V6ONLY cleared covers IPv4 too on
        // Linux/BSD; macOS and Windows need an explicit dual-stack decision, so
        // we just use IPv4 for the wildcard case -- it is the address a game
        // client binds to 99% of the time and avoids the platform split.
        auto* address       = reinterpret_cast<sockaddr_in*>(&bind_address);
        address->sin_family = AF_INET;
        address->sin_port   = htons(local_port);
        address->sin_addr.s_addr = INADDR_ANY;
        bind_length              = sizeof(sockaddr_in);
    }

    const SocketHandle handle = socket(bind_address.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidPlatformSocket)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "socket() failed: " + SocketErrorText(code));
        return false;
    }

    if (reuse_address)
    {
        const int enable = 1;
        // NOTE: on Windows this lets several sockets share a port (it behaves
        // like SO_REUSEADDR + SO_REUSEPORT on Linux). Set reuse_address=false
        // when exclusivity matters.
        setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enable), sizeof(enable));
    }

    if (bind(handle, reinterpret_cast<sockaddr*>(&bind_address), bind_length) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "bind() failed: " + SocketErrorText(code));
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        return false;
    }

    m_Handle     = FromHandle(handle);
    m_LastStatus = NetworkStatus::Success;
    m_LastError.clear();
    return true;
}

void UdpSocket::Close()
{
    if (m_Handle == kInvalidUdpSocketHandle)
    {
        return;
    }
    const SocketHandle handle = ToHandle(m_Handle);
    m_Handle                  = kInvalidUdpSocketHandle;
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
}

uint16_t UdpSocket::GetLocalPort() const
{
    if (!IsOpen())
    {
        return 0;
    }
    sockaddr_storage address {};
    socklen_t        length = sizeof(address);
    if (getsockname(ToHandle(m_Handle), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        return 0;
    }
    NetworkEndpoint endpoint;
    if (!SockaddrToEndpoint(address, endpoint))
    {
        return 0;
    }
    return endpoint.port;
}

// =============================================================================
// Configuration
// =============================================================================

bool UdpSocket::EnsureOpen() const
{
    return IsOpen();
}

bool UdpSocket::SetNonBlocking(bool enabled)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    const SocketHandle handle = ToHandle(m_Handle);
#if defined(_WIN32)
    u_long mode = enabled ? 1u : 0u;
    if (ioctlsocket(handle, FIONBIO, &mode) != 0)
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0)
    {
        SetFailure(NetworkStatus::IoFailure, "fcntl(F_GETFL) failed");
        return false;
    }
    const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(handle, F_SETFL, next) != 0)
#endif
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "non-blocking switch failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::SetBroadcast(bool enabled)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    const int enable = enabled ? 1 : 0;
    if (setsockopt(ToHandle(m_Handle), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&enable), sizeof(enable)) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "SO_BROADCAST failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::SetSendBufferSize(int bytes)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    const int size = bytes;
    if (setsockopt(ToHandle(m_Handle), SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "SO_SNDBUF failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::SetReceiveBufferSize(int bytes)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    const int size = bytes;
    if (setsockopt(ToHandle(m_Handle), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "SO_RCVBUF failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::JoinMulticastGroup(const std::string& group_address, const std::string& local_interface)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }

    int      family = AF_UNSPEC;
    in_addr  ipv4 {};
    in6_addr ipv6 {};
    if (!ParseLiteralAddress(group_address, family, ipv4, ipv6))
    {
        SetFailure(NetworkStatus::InvalidArgument,
                   "multicast group '" + group_address + "' is not a literal IP");
        return false;
    }

    const SocketHandle handle = ToHandle(m_Handle);
    const char*        option_data = nullptr;
    int                option_length = 0;
    int                level  = 0;
    int                option = 0;

    if (family == AF_INET)
    {
        ip_mreq request {};
        std::memset(&request, 0, sizeof(request));
        request.imr_multiaddr = ipv4;
        if (local_interface.empty())
        {
            request.imr_interface.s_addr = INADDR_ANY;
        }
        else if (inet_pton(AF_INET, local_interface.c_str(), &request.imr_interface) != 1)
        {
            SetFailure(NetworkStatus::InvalidArgument,
                       "multicast interface '" + local_interface + "' is not an IPv4 literal");
            return false;
        }
        option_data   = reinterpret_cast<const char*>(&request);
        option_length = sizeof(request);
        level         = IPPROTO_IP;
        option        = IP_ADD_MEMBERSHIP;
    }
    else
    {
        ipv6_mreq request {};
        std::memset(&request, 0, sizeof(request));
        request.ipv6mr_multiaddr = ipv6;
        if (local_interface.empty())
        {
            request.ipv6mr_interface = 0;
        }
        else
        {
            request.ipv6mr_interface = MulticastInterfaceIndex(local_interface);
            if (request.ipv6mr_interface == 0)
            {
                SetFailure(NetworkStatus::InvalidArgument,
                           "unknown multicast interface '" + local_interface + "'");
                return false;
            }
        }
        option_data   = reinterpret_cast<const char*>(&request);
        option_length = sizeof(request);
        level         = IPPROTO_IPV6;
        option        = IPV6_JOIN_GROUP;
    }

    if (setsockopt(handle, level, option, option_data, option_length) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "join multicast failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::LeaveMulticastGroup(const std::string& group_address, const std::string& local_interface)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }

    int      family = AF_UNSPEC;
    in_addr  ipv4 {};
    in6_addr ipv6 {};
    if (!ParseLiteralAddress(group_address, family, ipv4, ipv6))
    {
        SetFailure(NetworkStatus::InvalidArgument,
                   "multicast group '" + group_address + "' is not a literal IP");
        return false;
    }

    const SocketHandle handle  = ToHandle(m_Handle);
    const char*        option_data = nullptr;
    int                option_length = 0;
    int                level  = 0;
    int                option = 0;

    if (family == AF_INET)
    {
        ip_mreq request {};
        std::memset(&request, 0, sizeof(request));
        request.imr_multiaddr         = ipv4;
        request.imr_interface.s_addr  = INADDR_ANY;
        if (!local_interface.empty())
        {
            (void)inet_pton(AF_INET, local_interface.c_str(), &request.imr_interface);
        }
        option_data   = reinterpret_cast<const char*>(&request);
        option_length = sizeof(request);
        level         = IPPROTO_IP;
        option        = IP_DROP_MEMBERSHIP;
    }
    else
    {
        ipv6_mreq request {};
        std::memset(&request, 0, sizeof(request));
        request.ipv6mr_multiaddr = ipv6;
        request.ipv6mr_interface = MulticastInterfaceIndex(local_interface);
        option_data   = reinterpret_cast<const char*>(&request);
        option_length = sizeof(request);
        level         = IPPROTO_IPV6;
        option        = IPV6_LEAVE_GROUP;
    }

    if (setsockopt(handle, level, option, option_data, option_length) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "leave multicast failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

bool UdpSocket::SetMulticastTtl(int hops)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    const SocketHandle handle = ToHandle(m_Handle);
    // Both options take a 32-bit int; IPV6_MULTICAST_HOPS is the v6 spelling.
    int value = hops;
    if (setsockopt(handle, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0 &&
        setsockopt(handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
    {
        const int code = LastSocketError();
        SetFailure(NetworkStatus::IoFailure, "multicast TTL failed: " + SocketErrorText(code));
        return false;
    }
    return true;
}

// =============================================================================
// I/O
// =============================================================================

bool UdpSocket::SendTo(const NetworkEndpoint& destination, const void* data, std::size_t size)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return false;
    }
    if (data == nullptr && size > 0)
    {
        SetFailure(NetworkStatus::InvalidArgument, "null payload");
        return false;
    }
    if (!destination.IsValid())
    {
        SetFailure(NetworkStatus::InvalidArgument, "destination endpoint has no address");
        return false;
    }

    sockaddr_storage address {};
    socklen_t        length = 0;
    if (!EndpointToSockaddr(destination, address, length))
    {
        SetFailure(NetworkStatus::DnsFailure, "cannot resolve '" + destination.address + "'");
        return false;
    }

    const SocketSsize sent = sendto(ToHandle(m_Handle),
                                static_cast<const char*>(data),
                                size,
                                kSendFlags,
                                reinterpret_cast<sockaddr*>(&address),
                                length);
    if (sent < 0)
    {
        const int code = LastSocketError();
        if (WouldBlock(code))
        {
            // Socket buffer full: not a failure, the caller should retry later.
            m_LastStatus = NetworkStatus::Success;
            m_LastError.clear();
            return false;
        }
        SetFailure(NetworkStatus::IoFailure, "sendto() failed: " + SocketErrorText(code));
        return false;
    }

    m_LastStatus = NetworkStatus::Success;
    m_LastError.clear();
    return true;
}

int UdpSocket::ReceiveFrom(void* buffer, std::size_t capacity, NetworkEndpoint& out_from)
{
    if (!EnsureOpen())
    {
        SetFailure(NetworkStatus::NotInitialized, "socket is not open");
        return -1;
    }
    if (buffer == nullptr || capacity == 0)
    {
        SetFailure(NetworkStatus::InvalidArgument, "null receive buffer");
        return -1;
    }

    sockaddr_storage from {};
    socklen_t        from_length = sizeof(from);
    const SocketSsize received   = recvfrom(ToHandle(m_Handle),
                                         static_cast<char*>(buffer),
                                         capacity,
                                         0,
                                         reinterpret_cast<sockaddr*>(&from),
                                         &from_length);
    if (received < 0)
    {
        const int code = LastSocketError();
        if (WouldBlock(code) || SoftRecvError(code))
        {
            m_LastStatus = NetworkStatus::Success;
            m_LastError.clear();
            return 0;
        }
        SetFailure(NetworkStatus::IoFailure, "recvfrom() failed: " + SocketErrorText(code));
        return -1;
    }

    out_from = NetworkEndpoint();
    SockaddrToEndpoint(from, out_from);

    m_LastStatus = NetworkStatus::Success;
    m_LastError.clear();
    return static_cast<int>(received);
}

void UdpSocket::SetFailure(NetworkStatus status, const std::string& message)
{
    m_LastStatus = status;
    m_LastError  = message;
}
