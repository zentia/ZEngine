#include "ShaderLabCacheManifest.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>

namespace ZEngine::ShaderLab
{

    namespace
    {

        // Suffix appended to a cache blob's filename to form its manifest.
        constexpr const char* kManifestSuffix = ".dep";

        // last_write_time() as an integer number of microseconds, or 0 when
        // the file cannot be stat'ed.
        //
        // Microseconds, not nanoseconds: MSVC's file_time_type counts 100ns
        // ticks since 1601, and widening that to nanoseconds overflows int64
        // around the year 1893. Microseconds keep ~292,000 years of headroom
        // on every implementation while staying far finer than any real
        // filesystem timestamp granularity.
        int64_t FileMtimeMicros(const std::filesystem::path& p, std::error_code& ec)
        {
            const auto t = std::filesystem::last_write_time(p, ec);
            if (ec)
            {
                return 0;
            }
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());
        }

        // Strips a trailing CR so a manifest written on Windows reads back
        // correctly on platforms whose text mode does no translation.
        void StripTrailingCR(std::string& s)
        {
            if (!s.empty() && s.back() == '\r')
            {
                s.pop_back();
            }
        }

        std::string ReadFileToString(const std::filesystem::path& p, bool* out_opened = nullptr)
        {
            std::ifstream ifs(p, std::ios::binary);
            if (out_opened)
            {
                *out_opened = ifs.is_open();
            }
            if (!ifs.is_open())
            {
                return {};
            }
            std::string out;
            std::string line;
            while (std::getline(ifs, line))
            {
                StripTrailingCR(line);
                out += line;
                out += '\n';
            }
            return out;
        }

    }  // namespace

    std::filesystem::path CacheManifestPathFor(const std::filesystem::path& cache_file)
    {
        std::filesystem::path manifest = cache_file;
        manifest += kManifestSuffix;
        return manifest;
    }

    bool WriteCacheManifest(const std::filesystem::path& cache_file,
                            const std::vector<std::filesystem::path>& dependencies,
                            std::string* out_error)
    {
        if (cache_file.empty())
        {
            if (out_error)
            {
                *out_error = "empty cache file path";
            }
            return false;
        }

        const std::filesystem::path manifest = CacheManifestPathFor(cache_file);

        std::error_code ec;
        std::filesystem::create_directories(manifest.parent_path(), ec);
        // create_directories failing is not fatal -- the directory may already
        // exist, and if it does not, the ofstream below fails and we report it.

        // Binary so the '\n' we write is the '\n' we get back on every host;
        // the reader strips CR defensively anyway.
        std::ofstream ofs(manifest, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            if (out_error)
            {
                *out_error = "cannot open manifest for writing: " + manifest.generic_string();
            }
            return false;
        }

        // Dedupe: a header reached through two #include paths is one input.
        // Order of first appearance is preserved so the manifest stays stable
        // (and diffable) between runs.
        std::set<std::filesystem::path> seen;
        size_t written = 0;
        for (const auto& dep : dependencies)
        {
            if (dep.empty())
            {
                continue;
            }
            if (!seen.insert(dep).second)
            {
                continue;
            }

            std::error_code mtime_ec;
            const int64_t mtime = FileMtimeMicros(dep, mtime_ec);
            // A dependency we cannot stat is still recorded, with mtime 0.
            // That makes the entry permanently stale -- which is the correct
            // outcome: we cannot prove the blob matches its inputs, so we
            // recompile until we can.
            ofs << mtime << '\t' << dep.generic_string() << '\n';
            ++written;
        }

        if (!ofs.good())
        {
            if (out_error)
            {
                *out_error = "write failed: " + manifest.generic_string();
            }
            return false;
        }

        if (written == 0)
        {
            if (out_error)
            {
                *out_error = "no dependencies recorded for " + cache_file.generic_string();
            }
            return false;
        }

        return true;
    }

    CacheManifestStatus ValidateCacheManifest(const std::filesystem::path& cache_file,
                                              std::string* out_reason)
    {
        if (cache_file.empty())
        {
            if (out_reason)
            {
                *out_reason = "empty cache file path";
            }
            return CacheManifestStatus::Stale;
        }

        const std::filesystem::path manifest = CacheManifestPathFor(cache_file);

        std::error_code ec;
        if (!std::filesystem::exists(manifest, ec) || ec)
        {
            if (out_reason)
            {
                *out_reason = "no manifest next to cached blob (" + manifest.generic_string() + ")";
            }
            return CacheManifestStatus::Missing;
        }

        bool opened = false;
        const std::string text = ReadFileToString(manifest, &opened);
        if (!opened)
        {
            if (out_reason)
            {
                *out_reason = "cannot read manifest: " + manifest.generic_string();
            }
            return CacheManifestStatus::Stale;
        }

        size_t checked = 0;
        size_t pos = 0;
        while (pos < text.size())
        {
            const size_t nl = text.find('\n', pos);
            const size_t end = (nl == std::string::npos) ? text.size() : nl;
            std::string line = text.substr(pos, end - pos);
            StripTrailingCR(line);

            if (!line.empty())
            {
                const size_t tab = line.find('\t');
                if (tab == std::string::npos)
                {
                    if (out_reason)
                    {
                        *out_reason = "malformed manifest line " + std::to_string(checked + 1);
                    }
                    return CacheManifestStatus::Stale;
                }

                const std::string mtime_text = line.substr(0, tab);
                const std::filesystem::path dep = line.substr(tab + 1);
                if (dep.empty() || mtime_text.empty())
                {
                    if (out_reason)
                    {
                        *out_reason = "malformed manifest line " + std::to_string(checked + 1);
                    }
                    return CacheManifestStatus::Stale;
                }

                const int64_t recorded = std::strtoll(mtime_text.c_str(), nullptr, 10);

                std::error_code dep_ec;
                if (!std::filesystem::exists(dep, dep_ec) || dep_ec)
                {
                    if (out_reason)
                    {
                        *out_reason = "dependency vanished: " + dep.generic_string();
                    }
                    return CacheManifestStatus::Stale;
                }

                const int64_t current = FileMtimeMicros(dep, dep_ec);
                if (dep_ec || current != recorded)
                {
                    if (out_reason)
                    {
                        *out_reason = "dependency changed: " + dep.generic_string();
                    }
                    return CacheManifestStatus::Stale;
                }

                ++checked;
            }

            if (nl == std::string::npos)
            {
                break;
            }
            pos = nl + 1;
        }

        if (checked == 0)
        {
            if (out_reason)
            {
                *out_reason = "manifest recorded no dependencies";
            }
            return CacheManifestStatus::Stale;
        }

        return CacheManifestStatus::Fresh;
    }

}  // namespace ZEngine::ShaderLab
