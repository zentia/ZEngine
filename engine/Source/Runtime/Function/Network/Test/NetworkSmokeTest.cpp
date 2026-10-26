// =============================================================================
// P0-6: network layer smoke test.
//
// Runs headless and OFFLINE: every check either stays inside the process or
// talks to loopback, so a CI machine with no internet (and no DNS) still gets
// a green run. The only exception is the opt-in live section at the bottom,
// which does nothing unless ZENGINE_NETWORK_SMOKE_HTTP_URL is set.
//
//   cmake -B build -DZENGINE_BUILD_NETWORK_SMOKE_TEST=ON
//   cmake --build build --target NetworkSmokeTest
//   ctest -R NetworkSmokeTest
//
// Same "own main()" / side-target pattern as Function/Audio/Test: the source is
// carved out of the ZRuntime glob in Runtime/CMakeLists.txt so /WHOLEARCHIVE
// cannot drag its main() into every consumer executable (LNK2005).
//
// A curl-less build (Web) is expected to pass too: the transports report
// NetworkStatus::Unsupported there, and every status assertion below accepts
// Unsupported as the alternative outcome.
// =============================================================================

#include "Runtime/Function/Network/NetworkCommon.h"
#include "Runtime/Function/Network/NetworkSystem.h"
#include "Runtime/Function/Network/UdpSocket.h"
#include "Runtime/Function/Network/WebSocket.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using ListenFd = SOCKET;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using ListenFd = int;
#endif

namespace
{
    int g_checks   = 0;
    int g_failures = 0;

    /// Filled by the "completion queued but never pumped" request below, and
    /// asserted after Shutdown(): a callback that was never delivered must stay
    /// undelivered, not fire during teardown.
    int g_dropped_by_shutdown = 0;

    void Check(bool condition, const char* what)
    {
        ++g_checks;
        if (condition)
        {
            std::printf("  ok   %s\n", what);
        }
        else
        {
            std::printf("  FAIL %s\n", what);
            ++g_failures;
        }
    }

    void Section(const char* name)
    {
        std::printf("\n[%s]\n", name);
    }

    /// NetworkSystem keeps Initialize()/Shutdown() protected -- only
    /// SystemRegistry is supposed to drive them -- so the test reaches them
    /// through a subclass, exactly as production registration does.
    class TestableNetworkSystem : public NetworkSystem
    {
    public:
        bool Boot() { return Initialize(); }
        void Halt() { Shutdown(); }
    };

    HttpRequestDesc DirectLoopbackDesc(const std::string& url)
    {
        HttpRequestDesc desc;
        desc.url = url;
        // A proxy picked up from http_proxy/https_proxy would swallow a
        // loopback request and turn every expectation below into a 502.
        desc.ignore_environment_proxy = true;
        desc.connect_timeout_seconds  = 2;
        desc.timeout_seconds          = 5;
        return desc;
    }

    // -------------------------------------------------------------------------
    // A TCP listener that completes handshakes but never answers.
    //
    // Listening with a backlog and *never calling accept()* is enough: the
    // kernel finishes the three-way handshake on its own, so curl's connect()
    // succeeds and the transfer then sits waiting for a response until its own
    // timeout fires. That is what makes the cancel and timeout checks below
    // deterministic instead of racing a connection-refused path that would
    // complete behind our back.
    // -------------------------------------------------------------------------
    class SilentListener
    {
    public:
        SilentListener()
        {
#if defined(_WIN32)
            WSADATA wsa {};
            (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
            m_Fd = ::socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32)
            if (m_Fd == INVALID_SOCKET)
#else
            if (m_Fd < 0)
#endif
            {
                return;
            }

            sockaddr_in address {};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;  // OS picks the port
            if (::bind(m_Fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            {
                Close();
                return;
            }
            if (::listen(m_Fd, 8) != 0)
            {
                Close();
                return;
            }

            sockaddr_in bound {};
#if defined(_WIN32)
            int length = sizeof(bound);
#else
            socklen_t length = sizeof(bound);
#endif
            if (::getsockname(m_Fd, reinterpret_cast<sockaddr*>(&bound), &length) == 0)
            {
                m_Port = ntohs(bound.sin_port);
            }
        }

        ~SilentListener() { Close(); }

        bool  IsListening() const { return m_Port != 0; }
        std::string Url() const { return "http://127.0.0.1:" + std::to_string(m_Port) + "/never"; }

    private:
        void Close()
        {
#if defined(_WIN32)
            if (m_Fd != INVALID_SOCKET)
            {
                ::closesocket(m_Fd);
                m_Fd = INVALID_SOCKET;
            }
#else
            if (m_Fd >= 0)
            {
                ::close(m_Fd);
                m_Fd = -1;
            }
#endif
        }

        ListenFd     m_Fd
#if defined(_WIN32)
            {INVALID_SOCKET};
#else
            {-1};
#endif
        uint16_t m_Port {0};
    };

    /// True when `status` is the expected outcome, allowing Unsupported as the
    /// answer on a build whose transport is absent.
    bool StatusIs(NetworkStatus status, NetworkStatus expected, bool has_backend)
    {
        if (status == expected)
        {
            return true;
        }
        return !has_backend && status == NetworkStatus::Unsupported;
    }
}  // namespace

int main()
{
    // -------------------------------------------------------------------------
    Section("endpoint");
    // -------------------------------------------------------------------------
    {
        NetworkEndpoint endpoint;
        Check(NetworkEndpoint::TryParse("127.0.0.1:9000", endpoint) &&
                  endpoint.address == "127.0.0.1" && endpoint.port == 9000,
              "TryParse \"127.0.0.1:9000\"");
        Check(NetworkEndpoint::TryParse("[::1]:443", endpoint) &&
                  endpoint.address == "::1" && endpoint.port == 443,
              "TryParse \"[::1]:443\" (IPv6 literal)");
        Check(NetworkEndpoint::TryParse("gateway.example.com:80", endpoint) &&
                  endpoint.address == "gateway.example.com",
              "TryParse hostname");
        Check(!NetworkEndpoint::TryParse("gateway.example.com", endpoint), "no port -> rejected");
        Check(!NetworkEndpoint::TryParse("gateway.example.com:", endpoint), "empty port -> rejected");
        Check(!NetworkEndpoint::TryParse(":9000", endpoint), "empty host -> rejected");
        Check(!NetworkEndpoint::TryParse("host:70000", endpoint), "port > 65535 -> rejected");
        Check(!NetworkEndpoint::TryParse("host:abc", endpoint), "non-numeric port -> rejected");
        Check(!NetworkEndpoint::TryParse("", endpoint), "empty string -> rejected");

        NetworkEndpoint round_trip {"10.0.0.7", 1234};
        Check(round_trip.ToString() == "10.0.0.7:1234", "ToString (IPv4)");
        NetworkEndpoint ipv6 {"::1", 1234};
        Check(ipv6.ToString() == "[::1]:1234", "ToString brackets IPv6");
        Check(NetworkEndpoint {"", 0}.IsValid() == false, "empty endpoint is invalid");
    }

    // -------------------------------------------------------------------------
    Section("status");
    // -------------------------------------------------------------------------
    {
        bool all_named = true;
        for (int value = 0; value <= static_cast<int>(NetworkStatus::Unknown); ++value)
        {
            const char* text = NetworkStatusToString(static_cast<NetworkStatus>(value));
            if (text == nullptr || text[0] == '\0')
            {
                all_named = false;
            }
        }
        Check(all_named, "every NetworkStatus has a name");
        Check(std::string(NetworkStatusToString(NetworkStatus::Success)) == "Success",
              "NetworkStatusToString(Success)");
    }

    // -------------------------------------------------------------------------
    Section("system");
    // -------------------------------------------------------------------------
    TestableNetworkSystem network;
    {
        HttpRequestDesc desc;
        desc.url = "http://127.0.0.1:1/";
        Check(network.SendAsync(desc) == kInvalidHttpHandle, "SendAsync before Initialize() is refused");
        Check(network.GetActiveRequestCount() == 0, "nothing is in flight before Initialize()");

        Check(network.Boot(), "Initialize()");
        std::printf("  ..   transport available: %s\n",
                    network.IsBackendAvailable() ? "yes" : "no (degraded)");
    }

    const bool has_backend = network.IsBackendAvailable();

    // -------------------------------------------------------------------------
    Section("http blocking");
    // -------------------------------------------------------------------------
    {
        // Port 1 on loopback is closed: connect() is refused immediately, which
        // is the one failure a machine with no network reproduces exactly.
        const HttpResponse refused = network.SendBlocking(DirectLoopbackDesc("http://127.0.0.1:1/nope"));
        Check(!refused.IsSuccess(), "closed port is not a success");
        Check(StatusIs(refused.status, NetworkStatus::ConnectFailure, has_backend),
              "closed port -> ConnectFailure");
        Check(!refused.error.empty(), "failure carries a backend error string");

        HttpRequestDesc empty;
        empty.ignore_environment_proxy = true;
        const HttpResponse malformed = network.SendBlocking(empty);
        Check(!malformed.IsSuccess(), "empty URL is not a success");
        Check(StatusIs(malformed.status, NetworkStatus::InvalidArgument, has_backend),
              "empty URL -> InvalidArgument");

        SilentListener silent;
        if (!silent.IsListening())
        {
            std::printf("  ..   cannot bind a loopback listener; timeout checks skipped\n");
        }
        else
        {
            HttpRequestDesc hanging = DirectLoopbackDesc(silent.Url());
            hanging.timeout_seconds = 1;
            const HttpResponse timed_out = network.SendBlocking(hanging);
            Check(!timed_out.IsSuccess(), "silent server is not a success");
            Check(StatusIs(timed_out.status, NetworkStatus::Timeout, has_backend),
                  "silent server -> Timeout");
        }
    }

    // -------------------------------------------------------------------------
    Section("http async");
    // -------------------------------------------------------------------------
    {
        // 1. Completion is delivered by Pump(), on the calling thread.
        int completed = 0;
        const HttpHandle handle =
            network.SendAsync(DirectLoopbackDesc("http://127.0.0.1:1/nope"),
                              [&completed](const HttpResponse& response) {
                                  ++completed;
                                  Check(!response.IsSuccess(), "async completion is a failure");
                              });
        Check(handle != kInvalidHttpHandle, "SendAsync returned a handle");

        bool delivered = false;
        for (int attempt = 0; attempt < 100 && !delivered; ++attempt)
        {
            network.Pump();
            delivered = (completed == 1);
            if (!delivered)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        Check(delivered, "completion delivered by Pump()");
        Check(network.IsRequestDone(handle), "handle reports done");
        Check(network.GetActiveRequestCount() == 0, "no requests left in flight");

        // 2. CancelRequest drops the callback rather than invoking it.
        SilentListener silent;
        if (!silent.IsListening())
        {
            std::printf("  ..   cannot bind a loopback listener; cancel check skipped\n");
        }
        else
        {
            int cancelled_calls = 0;
            HttpRequestDesc hanging = DirectLoopbackDesc(silent.Url());
            hanging.timeout_seconds = 30;  // long enough that the cancel always wins
            const HttpHandle pending = network.SendAsync(hanging, [&cancelled_calls](const HttpResponse&) {
                ++cancelled_calls;
            });
            Check(pending != kInvalidHttpHandle, "SendAsync(hanging) returned a handle");
            Check(network.CancelRequest(pending), "CancelRequest accepted");
            for (int attempt = 0; attempt < 25; ++attempt)
            {
                network.Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Check(cancelled_calls == 0, "cancelled request never invokes its callback");
            Check(network.IsRequestDone(pending), "cancelled handle reports done");
        }

        // 3. Queue a completion and deliberately never pump it; the shutdown
        //    section asserts it was discarded rather than delivered.
        const HttpHandle undelivered =
            network.SendAsync(DirectLoopbackDesc("http://127.0.0.1:1/nope"),
                              [](const HttpResponse&) { ++g_dropped_by_shutdown; });
        Check(undelivered != kInvalidHttpHandle, "SendAsync before Shutdown()");
    }

    // -------------------------------------------------------------------------
    Section("udp");
    // -------------------------------------------------------------------------
    {
        auto receiver = network.CreateUdpSocket();
        auto sender   = network.CreateUdpSocket();
        Check(receiver->OpenBound("127.0.0.1", 0), "receiver bound to loopback");
        Check(sender->OpenBound("127.0.0.1", 0), "sender bound to loopback");
        Check(receiver->GetLocalPort() != 0, "receiver got an OS-assigned port");
        Check(receiver->SetNonBlocking(true), "receiver SetNonBlocking(true)");
        Check(sender->SetNonBlocking(true), "sender SetNonBlocking(true)");

        NetworkEndpoint destination {"127.0.0.1", receiver->GetLocalPort()};
        const char*     payload = "zengine-udp";
        Check(sender->SendTo(destination, payload, std::strlen(payload)), "SendTo accepted");

        char     buffer[64] {};
        NetworkEndpoint from;
        int      received = 0;
        for (int attempt = 0; attempt < 100 && received <= 0; ++attempt)
        {
            received = receiver->ReceiveFrom(buffer, sizeof(buffer), from);
            if (received <= 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        Check(received == static_cast<int>(std::strlen(payload)), "datagram received intact");
        Check(std::string(buffer, received > 0 ? received : 0) == payload, "payload matches");
        Check(from.address == "127.0.0.1", "sender address recovered");
        Check(from.port == sender->GetLocalPort(), "sender port recovered");

        const int empty = receiver->ReceiveFrom(buffer, sizeof(buffer), from);
        Check(empty == 0, "empty queue returns 0 (would block), not -1");

        NetworkEndpoint bogus {"this-host-does-not-exist.invalid", 1234};
        Check(!sender->SendTo(bogus, payload, std::strlen(payload)), "unresolvable destination rejected");
    }

    // -------------------------------------------------------------------------
    Section("websocket");
    // -------------------------------------------------------------------------
    {
        WebSocketDesc desc;
        desc.url                       = "ws://127.0.0.1:1/";
        desc.ignore_environment_proxy  = true;
        desc.connect_timeout_seconds   = 2;

        auto socket = network.CreateWebSocket(desc);
        Check(socket != nullptr, "CreateWebSocket returned a socket");
        if (socket != nullptr)
        {
            for (int attempt = 0; attempt < 250 && socket->GetState() != WebSocketState::Closed;
                 ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Check(socket->GetState() == WebSocketState::Closed, "refused upgrade ends Closed");
            Check(StatusIs(socket->GetLastStatus(), NetworkStatus::ConnectFailure, has_backend),
                  "refused upgrade -> ConnectFailure");
            Check(socket->GetUpgradeStatus() == 0, "no 101 was seen");
            Check(!socket->GetLastError().empty(), "failure carries a reason");
            socket->Abort();
        }
    }

    // -------------------------------------------------------------------------
    Section("live (opt-in)");
    // -------------------------------------------------------------------------
    {
        const char* live_url = std::getenv("ZENGINE_NETWORK_SMOKE_HTTP_URL");
        if (live_url == nullptr || live_url[0] == '\0')
        {
            std::printf("  ..   skipped (set ZENGINE_NETWORK_SMOKE_HTTP_URL to enable)\n");
        }
        else
        {
            const HttpResponse response = network.SendBlocking(DirectLoopbackDesc(live_url));
            std::printf("  ..   %s -> HTTP %ld (%s)\n", live_url,
                        static_cast<long>(response.http_status), response.error.c_str());
            Check(response.IsSuccess(), "live GET succeeded");
            Check(response.IsHttpSuccess(), "live GET answered 2xx");
        }
    }

    // -------------------------------------------------------------------------
    Section("shutdown");
    // -------------------------------------------------------------------------
    {
        network.Halt();
        Check(true, "Shutdown() returned cleanly");
        Check(network.GetActiveRequestCount() == 0, "no requests survive Shutdown()");
        Check(network.GetWebSocketCount() == 0, "no sockets survive Shutdown()");
        network.Pump();
        Check(g_dropped_by_shutdown == 0, "undelivered completions are discarded, not fired");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
