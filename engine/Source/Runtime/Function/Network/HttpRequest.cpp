#include "Runtime/Function/Network/HttpRequest.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#if defined(ZENGINE_NETWORK_HAS_CURL)
    #include <curl/curl.h>
#endif

// =============================================================================
// HttpResponse
// =============================================================================

std::string HttpResponse::GetHeader(const std::string& name) const
{
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = headers.find(lowered);
    return it == headers.end() ? std::string() : it->second;
}

namespace
{
    std::string TrimHeaderText(const std::string& text)
    {
        const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
        size_t     begin = 0;
        while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin])))
        {
            ++begin;
        }
        size_t end = text.size();
        while (end > begin && is_space(static_cast<unsigned char>(text[end - 1])))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    std::string LowerHeaderName(const std::string& text)
    {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
}  // namespace

// =============================================================================
// Construction / teardown
// =============================================================================

HttpRequest::HttpRequest() = default;

HttpRequest::~HttpRequest()
{
    ReleaseHandle();
}

void HttpRequest::ReleaseHandle()
{
    if (m_OutputFile != nullptr)
    {
        std::fclose(m_OutputFile);
        m_OutputFile = nullptr;
    }
#if defined(ZENGINE_NETWORK_HAS_CURL)
    if (m_HeaderList != nullptr)
    {
        curl_slist_free_all(m_HeaderList);
        m_HeaderList = nullptr;
    }
    if (m_Curl != nullptr)
    {
        curl_easy_cleanup(m_Curl);
        m_Curl = nullptr;
    }
#endif
    m_Running.store(false, std::memory_order_release);
}

void HttpRequest::Reset()
{
    ReleaseHandle();
    m_Response = HttpResponse();
    m_Downloaded.store(0, std::memory_order_release);
    m_Uploaded.store(0, std::memory_order_release);
    m_TotalDownload.store(0, std::memory_order_release);
    m_TotalUpload.store(0, std::memory_order_release);
    m_AbortedBySizeCap.store(false, std::memory_order_release);
}

// =============================================================================
// Starting a transfer
// =============================================================================

bool HttpRequest::Start(const HttpRequestDesc& desc)
{
    Reset();
    m_Desc = desc;

    if (desc.url.empty())
    {
        m_Response.status = NetworkStatus::InvalidArgument;
        m_Response.error  = "empty URL";
        return false;
    }

#if !defined(ZENGINE_NETWORK_HAS_CURL)
    m_Response.status = NetworkStatus::Unsupported;
    m_Response.error  = "HTTP is unavailable: this build has no libcurl";
    return false;
#else
    m_Curl = curl_easy_init();
    if (m_Curl == nullptr)
    {
        m_Response.status = NetworkStatus::NotInitialized;
        m_Response.error  = "curl_easy_init() failed; is curl_global_init() done?";
        return false;
    }

    std::memset(m_ErrorBuffer, 0, sizeof(m_ErrorBuffer));

    // CURLOPT_NOSIGNAL is mandatory: libcurl otherwise raises SIGPIPE/SIGALRM
    // from the DNS resolver threads, and NetworkSystem drives transfers from a
    // worker thread that has no handler installed.
    curl_easy_setopt(m_Curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_Curl, CURLOPT_ERRORBUFFER, m_ErrorBuffer);
    curl_easy_setopt(m_Curl, CURLOPT_URL, m_Desc.url.c_str());
    curl_easy_setopt(m_Curl, CURLOPT_USERAGENT, m_Desc.user_agent.c_str());

    // ---- headers --------------------------------------------------------
    const bool has_content_type =
        m_Desc.headers.find("Content-Type") != m_Desc.headers.end() ||
        m_Desc.headers.find("content-type") != m_Desc.headers.end();

    if (!m_Desc.body.empty() && !m_Desc.content_type.empty() && !has_content_type)
    {
        m_HeaderList = curl_slist_append(m_HeaderList,
                                         ("Content-Type: " + m_Desc.content_type).c_str());
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

    // ---- method + body ---------------------------------------------------
    //
    // PUT/PATCH/DELETE go through CURLOPT_CUSTOMREQUEST rather than
    // CURLOPT_UPLOAD: UPLOAD would demand a READFUNCTION callback, and every
    // method we support carries its body in memory anyway.
    const bool has_body = !m_Desc.body.empty();
    switch (m_Desc.method)
    {
        case HttpMethod::Get:
            curl_easy_setopt(m_Curl, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::Head:
            curl_easy_setopt(m_Curl, CURLOPT_NOBODY, 1L);
            break;
        case HttpMethod::Post:
            curl_easy_setopt(m_Curl, CURLOPT_POST, 1L);
            break;
        case HttpMethod::Put:
        case HttpMethod::Patch:
        case HttpMethod::Delete:
            curl_easy_setopt(m_Curl, CURLOPT_CUSTOMREQUEST, HttpMethodToString(m_Desc.method));
            break;
    }
    if (has_body)
    {
        // COPYPOSTFIELDS copies into libcurl's own storage, so the easy handle
        // stays valid after m_Desc goes away (NetworkSystem keeps the request
        // alive on the worker thread while the caller's descriptor is long
        // gone). POSTFIELDSIZE is set explicitly because the payload is binary
        // and may contain NUL bytes -- without it libcurl would strlen() it.
        curl_easy_setopt(m_Curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<HttpTransferOffset>(m_Desc.body.size()));
        curl_easy_setopt(m_Curl, CURLOPT_COPYPOSTFIELDS, m_Desc.body.data());
    }

    // ---- redirects / TLS / timeouts --------------------------------------
    curl_easy_setopt(m_Curl, CURLOPT_FOLLOWLOCATION, m_Desc.follow_redirects ? 1L : 0L);
    if (m_Desc.follow_redirects)
    {
        curl_easy_setopt(m_Curl, CURLOPT_MAXREDIRS, static_cast<long>(m_Desc.max_redirects));
    }
    curl_easy_setopt(m_Curl, CURLOPT_SSL_VERIFYPEER, m_Desc.verify_peer ? 1L : 0L);
    curl_easy_setopt(m_Curl, CURLOPT_SSL_VERIFYHOST, m_Desc.verify_host ? 2L : 0L);
    if (!m_Desc.ca_bundle_path.empty())
    {
        curl_easy_setopt(m_Curl, CURLOPT_CAINFO, m_Desc.ca_bundle_path.c_str());
    }
    if (m_Desc.timeout_seconds > 0)
    {
        curl_easy_setopt(m_Curl, CURLOPT_TIMEOUT, m_Desc.timeout_seconds);
    }
    if (m_Desc.connect_timeout_seconds > 0)
    {
        curl_easy_setopt(m_Curl, CURLOPT_CONNECTTIMEOUT, m_Desc.connect_timeout_seconds);
    }
    if (m_Desc.accept_compressed)
    {
        // "" == "everything libcurl was built with".
        curl_easy_setopt(m_Curl, CURLOPT_ACCEPT_ENCODING, "");
    }

    // "" (rather than NULL) is how libcurl spells "no proxy at all", which is
    // what makes requests to a local test server work on a machine whose
    // http_proxy environment variable points somewhere else.
    if (m_Desc.ignore_environment_proxy)
    {
        curl_easy_setopt(m_Curl, CURLOPT_PROXY, "");
    }
    else if (!m_Desc.proxy.empty())
    {
        curl_easy_setopt(m_Curl, CURLOPT_PROXY, m_Desc.proxy.c_str());
    }

    // ---- destination -----------------------------------------------------
    if (!m_Desc.output_path.empty())
    {
        uint64_t resume_from = 0;
        if (m_Desc.resume_partial)
        {
            std::error_code ec;
            const auto      existing = std::filesystem::file_size(m_Desc.output_path, ec);
            if (!ec)
            {
                resume_from = existing;
            }
        }
        m_OutputFile = std::fopen(m_Desc.output_path.c_str(), resume_from > 0 ? "ab" : "wb");
        if (m_OutputFile == nullptr)
        {
            m_Response.status = NetworkStatus::IoFailure;
            m_Response.error  = "cannot open output file '" + m_Desc.output_path + "'";
            ReleaseHandle();
            return false;
        }
        if (resume_from > 0)
        {
            curl_easy_setopt(m_Curl, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<HttpTransferOffset>(resume_from));
            m_Downloaded.store(resume_from, std::memory_order_release);
        }
        curl_easy_setopt(m_Curl, CURLOPT_WRITEFUNCTION, &HttpRequest::WriteFileCallback);
        curl_easy_setopt(m_Curl, CURLOPT_WRITEDATA, this);
    }
    else
    {
        curl_easy_setopt(m_Curl, CURLOPT_WRITEFUNCTION, &HttpRequest::WriteBodyCallback);
        curl_easy_setopt(m_Curl, CURLOPT_WRITEDATA, this);
    }

    curl_easy_setopt(m_Curl, CURLOPT_HEADERFUNCTION, &HttpRequest::WriteHeaderCallback);
    curl_easy_setopt(m_Curl, CURLOPT_HEADERDATA, this);

    curl_easy_setopt(m_Curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(m_Curl, CURLOPT_XFERINFOFUNCTION, &HttpRequest::ProgressCallback);
    curl_easy_setopt(m_Curl, CURLOPT_XFERINFODATA, this);

    m_Response.status = NetworkStatus::Pending;
    return true;
#endif
}

// =============================================================================
// Blocking driver
// =============================================================================

bool HttpRequest::PerformBlocking(const HttpRequestDesc& desc, HttpResponse& out_response)
{
    if (m_Running.load(std::memory_order_acquire))
    {
        out_response        = HttpResponse();
        out_response.status = NetworkStatus::InvalidArgument;
        out_response.error  = "request is already running";
        return false;
    }

    if (!Start(desc))
    {
        out_response = m_Response;
        return false;
    }

#if defined(ZENGINE_NETWORK_HAS_CURL)
    m_Running.store(true, std::memory_order_release);
    const CURLcode code = curl_easy_perform(m_Curl);
    Finish(static_cast<int>(code), nullptr);
#endif

    out_response = m_Response;
    return m_Response.IsSuccess();
}

void HttpRequest::Cancel()
{
    m_Running.store(false, std::memory_order_release);
    // Nothing else to do: NetworkSystem removes the handle from its CURLM,
    // which is what actually stops the transfer. A blocking transfer is by
    // definition already on the calling thread.
}

// =============================================================================
// Finishing
// =============================================================================

void HttpRequest::Finish(int curl_result, const char* override_error)
{
#if defined(ZENGINE_NETWORK_HAS_CURL)
    if (m_Curl != nullptr)
    {
        long http_status = 0;
        curl_easy_getinfo(m_Curl, CURLINFO_RESPONSE_CODE, &http_status);
        m_Response.http_status = static_cast<int>(http_status);

        const char* effective = nullptr;
        if (curl_easy_getinfo(m_Curl, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr)
        {
            m_Response.effective_url = effective;
        }

        double total_time = 0.0;
        if (curl_easy_getinfo(m_Curl, CURLINFO_TOTAL_TIME, &total_time) == CURLE_OK)
        {
            m_Response.total_seconds = total_time;
        }

        // Prefer the byte count libcurl itself reports: it is the wire size
        // *before* decompression, which is what a progress bar wants, whereas
        // our write callback counts decompressed bytes.
        HttpTransferOffset size_download = 0;
        if (curl_easy_getinfo(m_Curl, CURLINFO_SIZE_DOWNLOAD_T, &size_download) == CURLE_OK &&
            size_download > 0)
        {
            m_Response.downloaded_bytes = static_cast<uint64_t>(size_download);
        }
        else
        {
            m_Response.downloaded_bytes = m_Downloaded.load(std::memory_order_acquire);
        }
    }
#endif

    if (m_AbortedBySizeCap.load(std::memory_order_acquire))
    {
        m_Response.status = NetworkStatus::TooLarge;
        m_Response.error  = "response exceeded " + std::to_string(m_Desc.max_response_bytes) + " bytes";
    }
    else if (override_error != nullptr)
    {
        m_Response.status = NetworkStatus::Unknown;
        m_Response.error  = override_error;
    }
    else
    {
        m_Response.status = TranslateCurlResult(curl_result);
        if (m_Response.status != NetworkStatus::Success)
        {
#if defined(ZENGINE_NETWORK_HAS_CURL)
            const char* curl_text = curl_easy_strerror(static_cast<CURLcode>(curl_result));
            m_Response.error      = m_ErrorBuffer[0] != '\0' ? std::string(m_ErrorBuffer)
                                                             : (curl_text != nullptr ? curl_text : "unknown error");
#else
            m_Response.error = "HTTP is unavailable: this build has no libcurl";
#endif
        }
    }

    ReleaseHandle();
}

void HttpRequest::OnTransferFinished(int curl_result)
{
    Finish(curl_result, nullptr);
}

NetworkStatus HttpRequest::TranslateCurlResult(int curl_result)
{
#if !defined(ZENGINE_NETWORK_HAS_CURL)
    (void)curl_result;
    return NetworkStatus::Unsupported;
#else
    switch (static_cast<CURLcode>(curl_result))
    {
        case CURLE_OK:
            return NetworkStatus::Success;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return NetworkStatus::DnsFailure;
        case CURLE_COULDNT_CONNECT:
        case CURLE_SSL_CONNECT_ERROR:
            return NetworkStatus::ConnectFailure;
        case CURLE_OPERATION_TIMEDOUT:
            return NetworkStatus::Timeout;
        // CURLE_SSL_CACERT is a deprecated alias of
        // CURLE_PEER_FAILED_VERIFICATION (curl.h:671) -- listing both would be
        // a duplicate case value.
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
            return NetworkStatus::TlsFailure;
        case CURLE_ABORTED_BY_CALLBACK:
            return NetworkStatus::Aborted;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_NOT_BUILT_IN:
            return NetworkStatus::Unsupported;
        case CURLE_URL_MALFORMAT:
        case CURLE_BAD_FUNCTION_ARGUMENT:
            return NetworkStatus::InvalidArgument;
        case CURLE_FILE_COULDNT_READ_FILE:
        case CURLE_WRITE_ERROR:
        case CURLE_READ_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
            return NetworkStatus::IoFailure;
        default:
            return NetworkStatus::Unknown;
    }
#endif
}

// =============================================================================
// libcurl callbacks
// =============================================================================

size_t HttpRequest::WriteBodyCallback(char* data, size_t size, size_t count, void* user_data)
{
    auto* request = static_cast<HttpRequest*>(user_data);
    return request == nullptr ? 0 : request->OnWriteBody(data, size * count);
}

size_t HttpRequest::WriteFileCallback(char* data, size_t size, size_t count, void* user_data)
{
    auto* request = static_cast<HttpRequest*>(user_data);
    if (request == nullptr || request->m_OutputFile == nullptr)
    {
        return 0;
    }
    const size_t bytes = size * count;
    const size_t written = std::fwrite(data, 1, bytes, request->m_OutputFile);
    if (written != bytes)
    {
        return 0;  // libcurl turns a short write into CURLE_WRITE_ERROR
    }
    request->m_Downloaded.fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

size_t HttpRequest::WriteHeaderCallback(char* data, size_t size, size_t count, void* user_data)
{
    auto* request = static_cast<HttpRequest*>(user_data);
    return request == nullptr ? 0 : request->OnWriteHeader(data, size * count);
}

int HttpRequest::ProgressCallback(void* user_data,
                                  HttpTransferOffset download_total,
                                  HttpTransferOffset downloaded,
                                  HttpTransferOffset upload_total,
                                  HttpTransferOffset uploaded)
{
    auto* request = static_cast<HttpRequest*>(user_data);
    return request == nullptr ? 0
                              : request->OnProgress(download_total, downloaded, upload_total, uploaded);
}

size_t HttpRequest::OnWriteBody(const char* data, size_t bytes)
{
    if (bytes == 0)
    {
        return 0;
    }
    const uint64_t projected = static_cast<uint64_t>(m_Response.body.size()) + bytes;
    if (projected > m_Desc.max_response_bytes)
    {
        // Returning a value != bytes makes libcurl abort with CURLE_WRITE_ERROR;
        // m_AbortedBySizeCap lets Finish() report the real reason instead.
        m_AbortedBySizeCap.store(true, std::memory_order_release);
        return bytes - 1;
    }
    m_Response.body.insert(m_Response.body.end(), data, data + bytes);
    m_Downloaded.fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

size_t HttpRequest::OnWriteHeader(const char* data, size_t bytes)
{
    std::string line(data, bytes);
    line = TrimHeaderText(line);
    // Skip the status line ("HTTP/1.1 200 OK"); with redirects it repeats.
    if (line.empty() || line.compare(0, 5, "HTTP/") == 0)
    {
        return bytes;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
    {
        return bytes;
    }
    const std::string key   = LowerHeaderName(TrimHeaderText(line.substr(0, colon)));
    const std::string value = TrimHeaderText(line.substr(colon + 1));
    if (!key.empty())
    {
        m_Response.headers[key] = value;
    }
    return bytes;
}

int HttpRequest::OnProgress(HttpTransferOffset download_total,
                            HttpTransferOffset downloaded,
                            HttpTransferOffset upload_total,
                            HttpTransferOffset uploaded)
{
    m_TotalDownload.store(download_total > 0 ? static_cast<uint64_t>(download_total) : 0,
                          std::memory_order_relaxed);
    m_TotalUpload.store(upload_total > 0 ? static_cast<uint64_t>(upload_total) : 0,
                        std::memory_order_relaxed);
    m_Uploaded.store(uploaded > 0 ? static_cast<uint64_t>(uploaded) : 0, std::memory_order_relaxed);
    if (m_Desc.output_path.empty())
    {
        // The write callback already tracks the memory destination; for file
        // downloads it is the only counter we have.
        m_Downloaded.store(downloaded > 0 ? static_cast<uint64_t>(downloaded) : 0,
                           std::memory_order_relaxed);
    }
    // Non-zero tells libcurl to abort the transfer (used by Cancel semantics),
    // so always return 0 and let the handle removal do the stopping.
    return 0;
}

HttpProgress HttpRequest::GetProgress() const
{
    HttpProgress progress;
    progress.downloaded     = m_Downloaded.load(std::memory_order_acquire);
    progress.uploaded       = m_Uploaded.load(std::memory_order_acquire);
    progress.total_download = m_TotalDownload.load(std::memory_order_acquire);
    progress.total_upload   = m_TotalUpload.load(std::memory_order_acquire);
    progress.finished       = !m_Running.load(std::memory_order_acquire);
    return progress;
}
