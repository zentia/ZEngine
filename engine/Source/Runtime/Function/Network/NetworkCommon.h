#pragma once

/**
 * @file NetworkCommon.h
 * @brief Types shared by every transport in the network layer.
 *
 * The three transports (HTTP, WebSocket, UDP) deliberately share this header
 * so that game code can pass an endpoint or report a failure without pulling
 * in <curl/curl.h> or <winsock2.h>. Nothing here depends on a backend.
 */

#include <cstdint>
#include <string>

// =============================================================================
// Endpoint
// =============================================================================

/// An address + port pair. `address` may be a dotted IPv4/IPv6 literal or a
/// hostname; hostname resolution happens per call inside the transports.
struct NetworkEndpoint
{
    std::string address;
    uint16_t    port {0};

    bool IsValid() const { return !address.empty(); }

    bool operator==(const NetworkEndpoint& other) const
    {
        return port == other.port && address == other.address;
    }
    bool operator!=(const NetworkEndpoint& other) const { return !(*this == other); }

    /// "127.0.0.1:9000" for IPv4, "[::1]:9000" for IPv6.
    std::string ToString() const
    {
        const bool needs_brackets = address.find(':') != std::string::npos;
        std::string result;
        if (needs_brackets)
        {
            result += '[';
        }
        result += address;
        if (needs_brackets)
        {
            result += ']';
        }
        result += ':';
        result += std::to_string(port);
        return result;
    }

    /// Parse "host:port" / "[::1]:port". Returns false when the text is not a
    /// well-formed endpoint (no port, empty host, port out of range).
    static bool TryParse(const std::string& text, NetworkEndpoint& out_endpoint)
    {
        if (text.empty())
        {
            return false;
        }

        std::string host     = text;
        std::string port_text;

        if (!text.empty() && text[0] == '[')
        {
            const size_t close = text.find(']');
            if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
            {
                return false;
            }
            host      = text.substr(1, close - 1);
            port_text = text.substr(close + 2);
        }
        else
        {
            const size_t colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
            {
                return false;
            }
            host      = text.substr(0, colon);
            port_text = text.substr(colon + 1);
        }

        if (host.empty() || port_text.empty())
        {
            return false;
        }
        for (char c : port_text)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
        }

        const long port = std::stol(port_text);
        if (port <= 0 || port > 65535)
        {
            return false;
        }

        out_endpoint.address = host;
        out_endpoint.port    = static_cast<uint16_t>(port);
        return true;
    }
};

// =============================================================================
// Status
// =============================================================================

/// Transport-agnostic failure/success code.
///
/// Deliberately coarser than libcurl's 100+ CURLcode values and than the
/// platform errno set: game code branches on "can I retry?" and "do I show the
/// player a network error dialog?", not on the exact socket reason. The precise
/// backend string is always kept alongside, in HttpResponse::error /
/// UdpSocket::GetLastError().
enum class NetworkStatus : uint8_t
{
    Success = 0,
    /// Still in flight. Only ever reported by the asynchronous HTTP path.
    Pending,
    /// The transport was used before NetworkSystem::Initialize() (or after
    /// Shutdown()). On a curl-less build (Web) every curl-backed call reports
    /// Unsupported instead.
    NotInitialized,
    InvalidArgument,
    /// Backend does not implement this operation (e.g. HTTP on a build without
    /// libcurl, or a scheme libcurl was compiled without).
    Unsupported,
    DnsFailure,
    ConnectFailure,
    Timeout,
    /// Certificate / TLS handshake / peer verification failure.
    TlsFailure,
    /// Response exceeded the caller's byte budget.
    TooLarge,
    /// Cancelled by the caller, or by NetworkSystem::Shutdown().
    Aborted,
    /// Local filesystem read/write failure (download-to-file) or socket I/O.
    IoFailure,
    Unknown
};

inline const char* NetworkStatusToString(NetworkStatus status)
{
    switch (status)
    {
        case NetworkStatus::Success:         return "Success";
        case NetworkStatus::Pending:         return "Pending";
        case NetworkStatus::NotInitialized:  return "NotInitialized";
        case NetworkStatus::InvalidArgument: return "InvalidArgument";
        case NetworkStatus::Unsupported:     return "Unsupported";
        case NetworkStatus::DnsFailure:      return "DnsFailure";
        case NetworkStatus::ConnectFailure:  return "ConnectFailure";
        case NetworkStatus::Timeout:         return "Timeout";
        case NetworkStatus::TlsFailure:      return "TlsFailure";
        case NetworkStatus::TooLarge:        return "TooLarge";
        case NetworkStatus::Aborted:         return "Aborted";
        case NetworkStatus::IoFailure:       return "IoFailure";
        case NetworkStatus::Unknown:         break;
    }
    return "Unknown";
}
