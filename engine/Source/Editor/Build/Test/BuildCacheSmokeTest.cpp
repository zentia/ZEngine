// =============================================================================
// BuildCache smoke test -- the incremental-build state behind packaging.
//
// BuildCache.cpp is deliberately engine-free (no Object, no LOG, no Transfer)
// exactly so this can run as a plain executable: no project, no device, no
// ZRuntime. Everything that decides whether a build can skip work lives in
// there, so a mistake is a silently stale bundle -- worth testing directly.
// =============================================================================

#include "Editor/Build/BuildCache.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(const bool ok, const char* what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("[FAIL] %s\n", what);
        }
        else
        {
            std::printf("[ ok ] %s\n", what);
        }
    }

    std::filesystem::path g_root;

    void WriteFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    /// Forces the stamp to differ without touching the bytes, so a test can
    /// prove the stamp -- not the content -- is what triggers a re-read.
    void TouchNewer(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (!ec)
        {
            std::filesystem::last_write_time(path, stamp + std::chrono::seconds(2), ec);
        }
    }

    void TestHashing()
    {
        BuildHasher empty;
        Check(empty.Hex() == "cbf29ce484222325", "H1: an empty hasher is the FNV-1a offset basis");
        Check(empty.Hex().size() == 16, "H1: the hash is 16 hex digits");

        BuildHasher a;
        a.Add("abc");
        BuildHasher b;
        b.Add("abc");
        Check(a.Hex() == b.Hex(), "H2: the same text hashes the same");

        BuildHasher c;
        c.Add("abd");
        Check(a.Hex() != c.Hex(), "H3: different text hashes differently");

        BuildHasher left;
        left.Add("ab").Add("c");
        BuildHasher right;
        right.Add("a").Add("bc");
        Check(left.Hex() != right.Hex(), "H4: a separator keeps ('ab','c') from colliding with ('a','bc')");

        BuildHasher sized;
        sized.AddBytes("abc", 3);
        Check(sized.Hex() == BuildHasher().AddBytes("abc", 3).Hex(), "H5: AddBytes is stable for the same bytes");
        Check(sized.Hex() != BuildHasher().AddBytes("ab", 2).Hex(), "H6: AddBytes sees the length, not just the data");
    }

    void TestStamps()
    {
        const std::filesystem::path missing = g_root / "nope.bin";
        const BuildFileStamp missing_stamp = BuildStampOf(missing);
        Check(!missing_stamp.exists, "S1: a missing file has no stamp");
        Check(BuildStampKey(missing_stamp).empty(), "S1: and therefore no stamp key");

        const std::filesystem::path present = g_root / "present.bin";
        WriteFile(present, "0123456789");
        const BuildFileStamp stamp = BuildStampOf(present);
        Check(stamp.exists, "S2: an existing file has a stamp");
        Check(stamp.size == 10, "S2: the stamp carries the file size");

        const std::string first = BuildStampKey(stamp);
        TouchNewer(present);
        Check(BuildStampKey(BuildStampOf(present)) != first, "S3: touching the file changes the stamp");

        WriteFile(present, "0123456789");  // same bytes, newer mtime on purpose
        Check(BuildStampOf(present).size == 10, "S3: the size is still what it was");
    }

    void TestContentHashReuse()
    {
        BuildCache cache;
        cache.Reset();

        const std::filesystem::path file = g_root / "asset.zasset";
        WriteFile(file, "payload-one");

        bool ok = false;
        const std::string first = cache.ContentHash("k", file, ok);
        Check(ok && !first.empty(), "C1: hashing an existing file succeeds");

        // Stamp unchanged => the cached hash is reused WITHOUT reading the file.
        // Poison the cached value: if the next call re-read the file it would
        // come back with the real hash instead of the poison.
        BuildCache poisoned = cache;
        poisoned.inputs["k"].hash = "deadbeefdeadbeef";
        bool reuse_ok = false;
        Check(poisoned.ContentHash("k", file, reuse_ok) == "deadbeefdeadbeef" && reuse_ok,
              "C2: an unchanged stamp reuses the cached hash instead of reading");

        // Bytes changed => the hash has to change even if the size did not.
        WriteFile(file, "payload-two");
        bool changed_ok = false;
        const std::string changed = cache.ContentHash("k", file, changed_ok);
        Check(changed_ok && changed != first, "C3: changed bytes change the hash (same length, different content)");

        // Content back to the original => back to the original hash. This is the
        // whole point of hashing instead of comparing timestamps: a checkout
        // that only moved mtimes must NOT rebuild.
        WriteFile(file, "payload-one");
        TouchNewer(file);
        bool restored_ok = false;
        Check(cache.ContentHash("k", file, restored_ok) == first && restored_ok,
              "C4: identical bytes hash identically even after the mtime moved");

        std::filesystem::remove(file);
        bool gone_ok = true;
        const std::string gone = cache.ContentHash("k", file, gone_ok);
        Check(!gone_ok && gone.empty(), "C5: an unreadable input reports failure (caller must rebuild)");
        Check(cache.inputs.find("k") == cache.inputs.end(), "C5: and forgets what it knew about it");
    }

    void TestUpToDate()
    {
        BuildCache cache;
        cache.Reset();

        const std::filesystem::path artifact = g_root / "level.zbundle";
        WriteFile(artifact, "0123456789");
        cache.Record("level", "fingerprint-1", 10);
        Check(cache.IsUpToDate("level", "fingerprint-1", artifact), "U1: a recorded, present artifact is up to date");

        Check(!cache.IsUpToDate("level", "fingerprint-2", artifact), "U2: a new fingerprint is not");
        Check(!cache.IsUpToDate("other", "fingerprint-1", artifact), "U2: an unrecorded key is not");

        std::filesystem::remove(artifact);
        Check(!cache.IsUpToDate("level", "fingerprint-1", artifact),
              "U3: a deleted artifact is not -- the cache never outranks the file");

        WriteFile(artifact, "0123456789_extra");
        Check(!cache.IsUpToDate("level", "fingerprint-1", artifact), "U3: and neither is a resized one");

        WriteFile(artifact, "0123456789");
        Check(cache.IsUpToDate("level", "fingerprint-1", artifact), "U3: restoring it makes it up to date again");
    }

    void TestStaleKeys()
    {
        BuildCache cache;
        cache.Reset();
        cache.Record("shared", "a", 1);
        cache.Record("level_1-1", "b", 2);
        cache.Record("level_1-2", "c", 3);

        const std::vector<std::string> stale = cache.StaleKeys({"shared", "level_1-1"});
        Check(stale.size() == 1 && stale.front() == "level_1-2", "T1: only artifacts nobody produced are stale");
        Check(cache.StaleKeys({"shared", "level_1-1", "level_1-2"}).empty(), "T1: a complete build leaves nothing");

        cache.Erase("level_1-2");
        Check(cache.outputs.find("level_1-2") == cache.outputs.end(), "T2: Erase drops the record");
    }

    void TestPersistence()
    {
        const std::filesystem::path cache_path = g_root / "cache" / "build_cache.json";

        BuildCache written;
        written.Reset();
        written.environment = "Standalone|1";
        const std::filesystem::path asset = g_root / "asset.zasset";
        WriteFile(asset, "bytes");
        bool ok = false;
        const std::string hash = written.ContentHash("asset", asset, ok);
        written.Record("level", "fp-level", 42);

        std::string save_error;
        Check(written.Save(cache_path, save_error), "P1: the cache saves");
        Check(std::filesystem::exists(cache_path), "P1: to the path it was given");

        BuildCache read_back;
        std::string load_error;
        Check(read_back.Load(cache_path, load_error), "P2: and loads back");
        Check(read_back.environment == "Standalone|1", "P2: environment survives");
        Check(read_back.inputs.count("asset") == 1 && read_back.inputs["asset"].hash == hash,
              "P2: input hashes survive");
        Check(read_back.outputs.count("level") == 1 && read_back.outputs["level"].fingerprint == "fp-level" &&
                  read_back.outputs["level"].size_bytes == 42,
              "P2: output fingerprints survive");

        BuildCache missing;
        std::string missing_error;
        Check(!missing.Load(g_root / "cache" / "does_not_exist.json", missing_error),
              "P3: a missing cache does not load");
        Check(missing.loaded(), "P3: but it is still usable -- a first build is not an error");
        Check(missing.outputs.empty() && missing.environment.empty(), "P3: it is simply empty");

        const std::filesystem::path corrupt = g_root / "corrupt.json";
        WriteFile(corrupt, "{ this is not json");
        BuildCache corrupt_cache;
        std::string corrupt_error;
        Check(!corrupt_cache.Load(corrupt, corrupt_error), "P4: a corrupt cache does not load");
        Check(!corrupt_error.empty(), "P4: and says why, so the caller can log it");

        const std::filesystem::path old_version = g_root / "old.json";
        WriteFile(old_version, "{\"version\": 999, \"environment\": \"\"}");
        BuildCache old_cache;
        std::string old_error;
        Check(!old_cache.Load(old_version, old_error), "P5: a cache from another version is discarded");
        Check(!old_cache.loaded(), "P5: and marked unusable so the caller rebuilds everything");
    }

    void TestReset()
    {
        BuildCache cache;
        cache.Reset();
        cache.environment = "x";
        cache.Record("a", "fp", 1);
        Check(!cache.outputs.empty(), "R1: a reset cache starts usable, not empty-by-accident");

        cache.Reset();
        Check(cache.environment.empty() && cache.outputs.empty() && cache.inputs.empty(),
              "R1: Reset clears everything");
        Check(cache.loaded(), "R1: and leaves it usable -- that is what a forced rebuild wants");
    }
}  // namespace

int main()
{
    g_root = std::filesystem::temp_directory_path() / "zengine_buildcache_smoke";
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
    std::filesystem::create_directories(g_root, ec);
    if (ec)
    {
        std::printf("cannot create the scratch directory %s: %s\n", g_root.generic_string().c_str(),
                    ec.message().c_str());
        return 2;
    }

    TestHashing();
    TestStamps();
    TestContentHashReuse();
    TestUpToDate();
    TestStaleKeys();
    TestPersistence();
    TestReset();

    std::filesystem::remove_all(g_root, ec);

    std::printf("\nBuildCache smoke test: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
