#pragma once

/**
 * @file HttpRequest.h
 * @brief One HTTP(S) transfer, built on libcurl's easy interface.
 *
 * The class is transport-only: it owns a CURL handle and knows nothing about
 * threads. Two drivers sit on top of it:
 *
 *   - PerformBlocking() runs curl_easy_perform() on the calling thread
 *     (editor tools, cook steps, smoke tests, NetworkSystem::SendBlocking);
 *   - NetworkSystem adds the handle to its CURLM and calls
 *     OnTransferFinished() from the worker thread once curl reports the
 *     transfer done.
 *
 * On builds without libcurl (Emscripten -- 3rdparty/CMakeLists.txt skips curl
 * there) every entry point reports NetworkStatus::Unsupported and the file
 * compiles to nothing; game code needs no #ifdefs, it just stops working, the
 * same deal AudioSystem makes for its null device.
 */

#include "Runtime/Function/Network/NetworkCommon.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(ZENGINE_NETWORK_HAS_CURL)
    #include <curl/curl.h>
#endif

/// libcurl's byte-count type (curl_off_t). Spelled out so the progress callback
/// keeps the exact signature curl's typechecking setopt macro expects, while
/// the curl-less build still compiles the class declaration.
#if defined(ZENGINE_NETWORK_HAS_CURL)
using HttpTransferOffset = curl_off_t;
#else
using HttpTransferOffset = long long;
#endif

// =============================================================================
// Descriptors
// =============================================================================

/// Opaque id for an in-flight request handed out by NetworkSystem::SendAsync().
/// Monotonic, so 0 is a safe "invalid" sentinel.
using HttpHandle = std::uint64_t;
inline constexpr HttpHandle kInvalidHttpHandle = 0;

enum class HttpMethod : uint8_t
{
    Get = 0,
    Head,
    Post,
    Put,
    Patch,
    Delete
};

inline const char* HttpMethodToString(HttpMethod method)
{
    switch (method)
    {
        case HttpMethod::Get:    return "GET";
        case HttpMethod::Head:   return "HEAD";
        case HttpMethod::Post:   return "POST";
        case HttpMethod::Put:    return "PUT";
        case HttpMethod::Patch:  return "PATCH";
        case HttpMethod::Delete: return "DELETE";
    }
    return "GET";
}

/// Everything needed to fire one request. Defaults are tuned for "fetch a
/// manifest from a CDN", which is what the engine does most of the time.
struct HttpRequestDesc
{
    std::string  url;

    HttpMethod   method {HttpMethod::Get};

    /// Sent verbatim (case preserved). Content-Type is added automatically
    /// from `content_type` when `body` is non-empty.
    std::unordered_map<std::string, std::string> headers;

    std::vector<uint8_t> body;

    /// "application/json", "application/octet-stream", ... Ignored when
    /// `body` is empty, or when a Content-Type header is already present.
    std::string  content_type;

    std::string  user_agent {"ZEngine"};

    /// Empty -> libcurl's built-in CA store. Point at a bundled cacert.pem on
    /// platforms that ship none (Android / iOS builds with CURL_ENABLE_SSL are
    /// the usual suspects).
    std::string  ca_bundle_path;

    /// Non-empty -> stream the body to this file instead of buffering it in
    /// memory. The parent directory must already exist.
    std::string  output_path;

    /// Append to an existing `output_path` (HTTP Range resume). No-op when
    /// `output_path` is empty.
    bool         resume_partial {false};

    bool         follow_redirects {true};
    uint32_t     max_redirects {5};

    bool         verify_peer {true};
    bool         verify_host {true};

    /// Whole-transfer timeout, seconds. 0 disables it (libcurl default).
    long         timeout_seconds {30};
    /// Connect phase only, seconds. Kept separate so a slow server does not
    /// eat the whole budget before a single byte arrives.
    long         connect_timeout_seconds {10};

    /// Hard cap on the response body. The transfer is aborted with
    /// NetworkStatus::TooLarge when exceeded -- a misbehaving CDN must not be
    /// able to OOM the process.
    uint64_t     max_response_bytes {64ull * 1024ull * 1024ull};

    /// Enable gzip/deflate/br decoding (libcurl sends Accept-Encoding for all
    /// the schemes it was built with).
    bool         accept_compressed {true};

    /// Explicit proxy, e.g. "http://proxy.internal:8080". Empty -> libcurl's
    /// default, which honours the http_proxy / https_proxy environment
    /// variables.
    std::string  proxy;

    /// Force a direct connection regardless of the environment. Needed for
    /// anything talking to a loopback server on a machine that has a proxy
    /// configured (dev machines and CI runners routinely do).
    bool         ignore_environment_proxy {false};
};

// =============================================================================
// Results
// =============================================================================

/// Byte counters updated by libcurl's progress callback. Read from any thread
/// while the transfer is running; treated as advisory (monotonic but not
/// necessarily fresh).
struct HttpProgress
{
    uint64_t downloaded {0};
    uint64_t uploaded {0};
    uint64_t total_download {0};
    uint64_t total_upload {0};
    bool     finished {false};

    /// 0..1 when the server sent a Content-Length, otherwise 0.
    float Fraction() const
    {
        if (total_download == 0)
        {
            return 0.0f;
        }
        return static_cast<float>(static_cast<double>(downloaded) / static_cast<double>(total_download));
    }
};

struct HttpResponse
{
    NetworkStatus status {NetworkStatus::Pending};

    /// HTTP status line code (200 / 404 / ...). 0 when the transfer never
    /// reached a response -- check `status` first.
    int        http_status {0};

    /// libcurl's human-readable reason, or our own ("response exceeded N
    /// bytes"). Empty on success.
    std::string error;

    /// Final URL after redirects.
    std::string effective_url;

    /// Response headers, keys lower-cased (HTTP header names are
    /// case-insensitive). Repeated headers keep the last value.
    std::unordered_map<std::string, std::string> headers;

    /// Body. Empty when the request wrote to `output_path`.
    std::vector<uint8_t> body;

    uint64_t downloaded_bytes {0};
    double   total_seconds {0.0};

    bool IsSuccess() const { return status == NetworkStatus::Success; }
    /// Success at the transport level *and* a 2xx from the server.
    bool IsHttpSuccess() const { return IsSuccess() && http_status >= 200 && http_status < 300; }

    std::string BodyAsString() const
    {
        return std::string(reinterpret_cast<const char*>(body.data()), body.size());
    }

    /// Case-insensitive header lookup, "" when absent.
    std::string GetHeader(const std::string& name) const;
};

/// Invoked once per request, always from the thread that calls
/// NetworkSystem::Pump() -- never from the network worker.
using HttpCompletionCallback = std::function<void(const HttpResponse&)>;

// =============================================================================
// HttpRequest
// =============================================================================

class HttpRequest
{
public:
    HttpRequest();
    ~HttpRequest();

    HttpRequest(const HttpRequest&)            = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;

    /// Build the easy handle and arm the transfer. Does not touch the network
    /// yet. Returns false (and fills the response) when `desc` is unusable.
    bool Start(const HttpRequestDesc& desc);

    /// Run the transfer on the calling thread and fill the response.
    /// Valid with or without a prior Start(); Start() is called on demand.
    bool PerformBlocking(const HttpRequestDesc& desc, HttpResponse& out_response);

    /// Ask libcurl to stop. Only meaningful for the multi path; blocking
    /// transfers are already on this thread.
    void Cancel();

    bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

    HttpProgress GetProgress() const;

    const HttpResponse&     GetResponse() const { return m_Response; }
    const HttpRequestDesc&  GetDesc() const { return m_Desc; }

    // ---- CURLM plumbing, used only by NetworkSystem ------------------------
#if defined(ZENGINE_NETWORK_HAS_CURL)
    CURL* GetCurlHandle() const { return m_Curl; }
#endif
    /// Called by NetworkSystem when curl_multi_info_read() reports this handle
    /// done. Serialises the result into `m_Response` and releases the handle.
    void OnTransferFinished(int curl_result);

    /// Collapse a CURLcode into the engine's coarser status enum.
    static NetworkStatus TranslateCurlResult(int curl_result);

private:
    void Reset();
    void Finish(int curl_result, const char* override_error);
    void ReleaseHandle();

    // libcurl callbacks (static trampolines into the instance).
    static size_t WriteBodyCallback(char* data, size_t size, size_t count, void* user_data);
    static size_t WriteFileCallback(char* data, size_t size, size_t count, void* user_data);
    static size_t WriteHeaderCallback(char* data, size_t size, size_t count, void* user_data);
    static int    ProgressCallback(void* user_data,
                                   HttpTransferOffset download_total,
                                   HttpTransferOffset downloaded,
                                   HttpTransferOffset upload_total,
                                   HttpTransferOffset uploaded);

    size_t OnWriteBody(const char* data, size_t bytes);
    size_t OnWriteHeader(const char* data, size_t bytes);
    int    OnProgress(HttpTransferOffset download_total,
                      HttpTransferOffset downloaded,
                      HttpTransferOffset upload_total,
                      HttpTransferOffset uploaded);

#if defined(ZENGINE_NETWORK_HAS_CURL)
    CURL*       m_Curl {nullptr};
    curl_slist* m_HeaderList {nullptr};
    char        m_ErrorBuffer[CURL_ERROR_SIZE] {'\0'};
#endif

    FILE*       m_OutputFile {nullptr};

    HttpRequestDesc m_Desc;
    HttpResponse    m_Response;

    std::atomic<uint64_t> m_Downloaded {0};
    std::atomic<uint64_t> m_Uploaded {0};
    std::atomic<uint64_t> m_TotalDownload {0};
    std::atomic<uint64_t> m_TotalUpload {0};
    std::atomic<bool>     m_Running {false};
    std::atomic<bool>     m_AbortedBySizeCap {false};
};
