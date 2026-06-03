// =====================================================================
// DerivedDataCacheSmokeTest
// ---------------------------------------------------------------------
// Standalone executable proving the LMDB-backed Derived Data Cache
// compiles, links, and round-trips. Exercises the IDerivedDataCache
// contract directly against a temp-directory LMDB env (no ProjectInfo
// required) plus the OpenDerivedDataCacheAt() accessor and the
// MakeDDCCacheKey() helper.
//
// Scenarios:
//   D1 put/get       : Put a value, get it back byte-for-byte.
//   D2 exists        : exists() true after Put, false for a fresh key.
//   D3 overwrite     : Put twice on same key -> last write wins.
//   D4 remove        : remove() then exists()==false and get()==false.
//   D5 accessor      : OpenDerivedDataCacheAt() returns a usable cache and
//                      the same pointer on re-open of the same dir.
//   D6 key helper    : MakeDDCCacheKey is deterministic and varies with input.
//
// Build: only when -DZENGINE_BUILD_DDC_SMOKE_TEST=ON.
// Exit codes: 0 all passed, 1 a scenario failed, 77 environment error.
// =====================================================================

#include "Runtime/Resource/Cache/DerivedDataCache.h"
#include "Runtime/Resource/Cache/DerivedDataCacheAccessor.h"
#include "Runtime/Resource/Cache/LMDBDerivedDataCache.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace Runtime;

namespace
{
    int g_failures = 0;
    void reportFail(const char* s, const char* d) { ++g_failures; std::fprintf(stderr, "[FAIL] %s: %s\n", s, d); }
    void reportOK(const char* s) { std::fprintf(stdout, "[ OK ] %s\n", s); }

    DDCValue makeValue(const std::vector<uint8_t>& bytes, uint32_t version)
    {
        DDCValue v;
        v.data = bytes;
        v.timestamp = std::time(nullptr);
        v.version = version;
        return v;
    }
}  // namespace

int main()
{
    std::fprintf(stderr,
                 "ZEngine DerivedDataCache smoke test\n"
                 "===================================\n");

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "zengine_ddc_smoke";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);  // start clean

    LMDBDerivedDataCache cache;
    if (!cache.Initialize(dir, /*max_size_mb*/ 64))
    {
        std::fprintf(stderr, "FATAL: LMDB Initialize failed at %s\n", dir.generic_string().c_str());
        return 77;
    }

    const DDCKey key {"Texture", "0123456789abcdef0123456789abcdef", "android_00000000deadbeef_v1"};
    const std::vector<uint8_t> payload {1, 2, 3, 4, 5, 250, 251, 252, 0, 255};

    // D1 put/get
    {
        if (!cache.Put(key, makeValue(payload, 1)))
        {
            reportFail("D1 put/get", "Put failed");
        }
        else
        {
            DDCValue got;
            if (!cache.get(key, got)) { reportFail("D1 put/get", "get failed"); }
            else if (got.data != payload || got.version != 1) { reportFail("D1 put/get", "value mismatch"); }
            else { reportOK("D1 put/get"); }
        }
    }

    // D2 exists
    {
        const DDCKey absent {"Texture", "ffffffffffffffffffffffffffffffff", "nope_v1"};
        if (cache.exists(key) && !cache.exists(absent)) { reportOK("D2 exists"); }
        else { reportFail("D2 exists", "exists() wrong for present/absent key"); }
    }

    // D3 overwrite (last write wins)
    {
        const std::vector<uint8_t> payload2 {9, 8, 7};
        cache.Put(key, makeValue(payload2, 2));
        DDCValue got;
        if (cache.get(key, got) && got.data == payload2 && got.version == 2) { reportOK("D3 overwrite"); }
        else { reportFail("D3 overwrite", "overwrite did not take"); }
    }

    // D4 remove
    {
        if (!cache.remove(key)) { reportFail("D4 remove", "remove failed"); }
        else
        {
            DDCValue got;
            if (!cache.exists(key) && !cache.get(key, got)) { reportOK("D4 remove"); }
            else { reportFail("D4 remove", "key still present after remove"); }
        }
    }

    cache.Shutdown();

    // D5 accessor round-trip + idempotent re-open
    {
        std::filesystem::path adir = std::filesystem::temp_directory_path() / "zengine_ddc_smoke_acc";
        std::filesystem::remove_all(adir, ec);
        IDerivedDataCache* c1 = OpenDerivedDataCacheAt(adir, 64);
        IDerivedDataCache* c2 = OpenDerivedDataCacheAt(adir, 64);
        if (c1 == nullptr) { reportFail("D5 accessor", "OpenDerivedDataCacheAt returned null"); }
        else if (c1 != c2) { reportFail("D5 accessor", "re-open of same dir returned a different cache"); }
        else
        {
            const DDCKey k {"Shader", "aaaa", "x_v1"};
            c1->Put(k, makeValue({42}, 7));
            DDCValue got;
            if (GetDerivedDataCache() != c1)
            {
                // GetDerivedDataCache may differ (no ProjectInfo here); not fatal.
            }
            if (c1->get(k, got) && got.data.size() == 1 && got.data[0] == 42) { reportOK("D5 accessor"); }
            else { reportFail("D5 accessor", "accessor cache get failed"); }
        }
        ShutdownDerivedDataCache();
    }

    // D6 key helper determinism
    {
        const std::string a = MakeDDCCacheKey("android", 0xdeadbeefcafef00dull, 1);
        const std::string b = MakeDDCCacheKey("android", 0xdeadbeefcafef00dull, 1);
        const std::string c = MakeDDCCacheKey("android", 0xdeadbeefcafef00dull, 2);  // diff version
        const std::string d = MakeDDCCacheKey("standalone", 0xdeadbeefcafef00dull, 1);  // diff platform
        if (a == b && a != c && a != d) { reportOK("D6 key helper"); }
        else { reportFail("D6 key helper", (a + " / " + c + " / " + d).c_str()); }
    }

    if (g_failures > 0)
    {
        std::fprintf(stderr, "\n%d scenario(s) FAILED.\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll scenarios passed.\n");
    return 0;
}
