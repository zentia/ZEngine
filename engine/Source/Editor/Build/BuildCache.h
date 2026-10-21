#pragma once

// =============================================================================
// BuildCache -- the persisted state that makes a packaging pass incremental.
//
// Two producers need it:
//   * AssetBundleBuilder           -- skip rewriting an unchanged .zbundle.
//   * TextureImporter::CookProjectTextures -- skip re-encoding an unchanged
//     texture, which is by far the most expensive step in a build.
//
// How "unchanged" is decided:
//
//   * The FINAL arbiter is a CONTENT HASH (FNV-1a 64) of every input, never a
//     timestamp: identical input bytes must produce an identical artifact, and
//     changed bytes must never be skipped.
//   * A timestamp is only a FAST PATH. (size, mtime) unchanged means the hash
//     recorded last time can be reused WITHOUT reading the file at all -- that
//     is what makes a no-op build cheap. When the stamp differs the file is
//     read and re-hashed, and if the bytes turn out to be identical after all
//     (a `git checkout` that only moved mtimes is the common case) the hash
//     still matches and the artifact is still skipped.
//   * The one theoretical hole is a file whose bytes changed while its size AND
//     mtime stayed identical -- that takes deliberately rewriting the
//     timestamp, and `force_rebuild` is the documented escape hatch.
//
// Everything fails SAFE: a missing, unreadable, half-written or wrong-version
// cache yields an empty cache and the caller rebuilds everything. A stale
// cache can never ship a stale artifact, it can only cost time.
//
// Deliberately free of engine dependencies (no Object, no LOG, no Transfer) so
// it can be exercised by a standalone smoke test.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/// Streaming FNV-1a 64. Not cryptographic -- it only has to answer "did any
/// input byte change", and it must give the same answer on every platform.
class BuildHasher
{
public:
    BuildHasher& AddBytes(const void* data, std::size_t size);
    /// Adds the text plus a separator, so ("ab","c") and ("a","bc") cannot
    /// collide. The only overload on purpose: an int/bool overload would make
    /// `Add(1)` ambiguous, and callers can say what they mean (`std::to_string`
    /// or `"1"/"0"`) far more cheaply than we can guess it.
    BuildHasher& Add(const std::string& text);

    /// 16 lowercase hex digits.
    std::string Hex() const;

    uint64_t state() const { return state_; }

private:
    uint64_t state_ = 0xcbf29ce484222325ull;  // FNV-1a 64 offset basis
};

/// "Did this file change?" Cheap to obtain, but only ever used to decide
/// whether the cached hash can be trusted -- never to decide the artifact is
/// up to date.
struct BuildFileStamp
{
    uint64_t size {0};
    /// Nanoseconds on whatever clock `std::filesystem::file_time_type` uses.
    /// Compared only against another stamp taken the same way, never shown to a
    /// user and never interpreted as a calendar time.
    int64_t mtime_ns {0};
    bool exists {false};
};

BuildFileStamp BuildStampOf(const std::filesystem::path& path);
std::string BuildStampKey(const BuildFileStamp& stamp);

/// Shared by every caller so there is one implementation, not one per pass.
bool BuildReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes);

/// One JSON file next to the artifacts it describes.
class BuildCache
{
public:
    /// Bump when the meaning of a fingerprint changes (a new input joins the
    /// hash, a writer format change, ...). A cache from another version is
    /// discarded wholesale.
    static constexpr int kVersion = 1;

    struct Entry
    {
        std::string stamp;
        std::string hash;
    };

    struct Output
    {
        std::string fingerprint;
        int64_t size_bytes {0};
    };

    /// False on missing / unreadable / corrupt / wrong-version. The cache is
    /// left empty either way, so the caller just rebuilds everything.
    bool Load(const std::filesystem::path& path, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;

    /// False when nothing was loaded -- including "the file did not exist
    /// yet", which is a first build, not an error.
    bool loaded() const { return loaded_; }

    /// When this differs from what was loaded the whole cache is dropped:
    /// platform, compression switch, content root, builder version -- anything
    /// that changes how inputs turn into outputs.
    std::string environment;

    std::unordered_map<std::string, Entry> inputs;
    std::unordered_map<std::string, Output> outputs;

    /// Throws everything away and marks the cache usable, which is what a
    /// forced full rebuild wants.
    void Reset();

    /// Content hash of `file`. Returns an empty string and `ok = false` when
    /// the file cannot be read -- the caller MUST then rebuild, because an
    /// unreadable input means the artifact cannot be reproduced.
    std::string ContentHash(const std::string& key, const std::filesystem::path& file, bool& ok);

    /// Same, for bytes the caller already has in memory (it just read them to
    /// pack them anyway). `stamp_key` may be empty when there is no file.
    std::string ContentHashKnown(const std::string& key, const std::string& stamp_key,
                                 const std::vector<uint8_t>& bytes);

    /// True when the artifact exists, has the recorded size and the
    /// fingerprint still matches.
    bool IsUpToDate(const std::string& key, const std::string& fingerprint,
                    const std::filesystem::path& artifact) const;

    void Record(const std::string& key, const std::string& fingerprint, int64_t size_bytes);
    void Erase(const std::string& key);

    /// Keys the cache knows about but this pass did not produce -- their
    /// artifacts are leftovers (a deleted level, a renamed asset) and must be
    /// deleted, or a player would keep loading content that no longer exists.
    std::vector<std::string> StaleKeys(const std::vector<std::string>& current_keys) const;

private:
    bool loaded_ {false};
};
