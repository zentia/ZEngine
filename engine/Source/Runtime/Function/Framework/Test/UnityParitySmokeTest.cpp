// =============================================================================
// Unity-parity smoke test.
//
//   cmake -B build -DZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST=ON
//   cmake --build build --target UnityParitySmokeTest
//   ctest --test-dir build -C Debug -R UnityParitySmokeTest
//
// Why one suite instead of one per feature
// ----------------------------------------
// Every new executable costs a CMakeLists.txt, a `option()`, two
// `list(REMOVE_ITEM ...)` entries in Runtime/CMakeLists.txt (SOURCE_FILES *and*
// REGULAR_SOURCE_FILES -- only one and /WHOLEARCHIVE drags the test's main()
// into ZRuntime and every consumer exe fails with LNK2005), and a `-D...=ON`
// in a workflow or the CI coverage ratchet (CiSmokeTestCoverageSmokeTest B3)
// goes red. The features in here are all small, all Unity-API-shaped and all
// headless, so they share one target and the boilerplate is paid once.
//
// Two groups
// ----------
//   A. Behaviour -- the rules themselves, tested against a real instance.
//   B. Wiring gate -- source-level: the rule exists in the engine at all.
//      A perfect implementation that is never called is indistinguishable from
//      no implementation, and it compiles and runs silently. This is the group
//      that catches that.
// =============================================================================

#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// The engine root is baked in by CMake so the wiring gate can read real source
// files without hard-coding a developer's drive letter.
#if !defined(ZENGINE_ENGINE_ROOT_DIR)
    #define ZENGINE_ENGINE_ROOT_DIR ""
#endif

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    std::string ReadFile(const std::string& rel_path)
    {
        const std::string full = std::string(ZENGINE_ENGINE_ROOT_DIR) + "/" + rel_path;
        std::ifstream in(full, std::ios::binary);
        if (!in)
        {
            return std::string();
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// Occurrences of `needle` in `haystack`, skipping nothing. Used by the
    /// wiring gate: counting beats `find() != npos` because a threshold can
    /// distinguish "declared" from "declared AND used".
    int CountOccurrences(const std::string& haystack, const char* needle)
    {
        int count = 0;
        std::string::size_type pos = 0;
        const std::string n(needle);
        while ((pos = haystack.find(n, pos)) != std::string::npos)
        {
            ++count;
            pos += n.size();
        }
        return count;
    }

    // ---------------------------------------------------------------- A ----
    void TestTags(TagLayerManager& m)
    {
        Check(m.HasTag("Untagged"), "A1", "the default tag is registered");
        Check(!m.HasTag(""), "A2", "the empty tag is never registered");
        Check(!m.HasTag("ThisTagWasNeverAdded"), "A3", "an unknown tag is not registered");

        Check(m.AddTag("Enemy"), "A4", "a new user tag can be added");
        Check(m.HasTag("Enemy"), "A5", "and is then registered");
        Check(!m.AddTag("Enemy"), "A6", "adding it twice is rejected (no duplicates)");
        Check(!m.AddTag(""), "A7", "adding the empty tag is rejected");

        Check(!m.RemoveTag("Untagged"), "A8", "a built-in tag cannot be removed");
        Check(m.HasTag("Untagged"), "A9", "and is still there afterwards");
        Check(m.RemoveTag("Enemy"), "A10", "a user tag can be removed");
        Check(!m.HasTag("Enemy"), "A11", "and is gone afterwards");
        Check(!m.RemoveTag("Enemy"), "A12", "removing it twice is rejected");

        const eastl::vector<eastl::string> tags = m.GetTags();
        Check(!tags.empty() && tags[0] == "Untagged", "A13", "Untagged is listed first");
        Check(m.GetTagCount() == static_cast<int>(tags.size()), "A14", "GetTagCount matches GetTags");
    }

    void TestLayers(TagLayerManager& m)
    {
        Check(m.LayerToName(0) == "Default", "A15", "layer 0 is Default");
        Check(m.LayerToName(5) == "UI", "A16", "layer 5 is UI");
        Check(m.NameToLayer("UI") == 5, "A17", "NameToLayer(\"UI\") is 5");
        Check(m.NameToLayer("Default") == 0, "A18", "NameToLayer(\"Default\") is 0");
        Check(m.NameToLayer("NoSuchLayer") == TagLayerManager::kInvalidLayer,
              "A19", "an unknown layer name is -1, not 0");

        Check(!m.SetLayerName(0, "Ground"), "A20", "a built-in layer cannot be renamed");
        Check(m.LayerToName(0) == "Default", "A21", "and keeps its name");
        Check(!m.SetLayerName(99, "Ground"), "A22", "an out-of-range layer cannot be named");
        Check(!m.SetLayerName(-1, "Ground"), "A23", "a negative layer cannot be named");

        Check(m.SetLayerName(8, "Ground"), "A24", "a user layer (8) can be named");
        Check(m.NameToLayer("Ground") == 8, "A25", "and resolves back to 8");
        Check(!m.SetLayerName(9, "Ground"), "A26", "a duplicate layer name is rejected");
        Check(m.NameToLayer("Ground") == 8, "A27", "so the name stays unambiguous");

        Check(!m.IsUserLayer(0) && !m.IsUserLayer(7), "A28", "0 and 7 are not user layers");
        Check(m.IsUserLayer(8) && m.IsUserLayer(31), "A29", "8 and 31 are user layers");
        Check(!m.IsUserLayer(32), "A30", "32 is out of range");
    }

    void TestMasks()
    {
        Check(TagLayerManager::LayerToMask(0) == 1u, "A31", "layer 0 is bit 0");
        Check(TagLayerManager::LayerToMask(5) == (1u << 5), "A32", "layer 5 is bit 5");
        Check(TagLayerManager::LayerToMask(31) == (1u << 31), "A33", "layer 31 is bit 31");
        Check(TagLayerManager::LayerToMask(32) == 0u, "A34", "layer 32 is 0, not UB from 1<<32");
        Check(TagLayerManager::LayerToMask(-1) == 0u, "A35", "a negative layer is 0");

        Check(TagLayerManager::MaskContainsLayer(1u << 5, 5), "A36", "a mask contains its own layer");
        Check(!TagLayerManager::MaskContainsLayer(1u << 5, 6), "A37", "and not the neighbour");
        Check(!TagLayerManager::MaskContainsLayer(0u, 0), "A38", "an empty mask contains nothing");
    }

    void TestCollisionMatrix(TagLayerManager& m)
    {
        // Fresh table: everything collides except Ignore Raycast.
        Check(!m.GetIgnoreLayerCollision(0, 1), "A39", "by default every pair collides");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer, 0),
              "A40", "Ignore Raycast is excluded from the default layer");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer,
                                        TagLayerManager::kIgnoreRaycastLayer),
              "A41", "and from itself");
        Check((m.GetCollisionMask(TagLayerManager::kIgnoreRaycastLayer) == 0u),
              "A42", "so its collision mask is empty");

        m.SetIgnoreLayerCollision(8, 9, true);
        Check(m.GetIgnoreLayerCollision(8, 9), "A43", "a pair can be excluded");
        Check(m.GetIgnoreLayerCollision(9, 8), "A44", "and the exclusion is symmetric");
        Check((m.GetCollisionMask(8) & (1u << 9)) == 0u, "A45", "layer 8's mask drops bit 9");
        Check((m.GetCollisionMask(9) & (1u << 8)) == 0u, "A46", "and layer 9's mask drops bit 8");
        Check((m.GetCollisionMask(8) & (1u << 10)) != 0u, "A47", "but keeps bit 10");

        m.SetIgnoreLayerCollision(8, 9, false);
        Check(!m.GetIgnoreLayerCollision(8, 9), "A48", "the exclusion can be lifted");
        Check(!m.GetIgnoreLayerCollision(9, 8), "A49", "and symmetrically");

        m.SetIgnoreLayerCollision(0, 1, true);
        m.ResetCollisionMatrix();
        Check(!m.GetIgnoreLayerCollision(0, 1), "A50", "ResetCollisionMatrix clears exclusions");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer, 0),
              "A51", "but Ignore Raycast stays excluded");
        Check(m.GetCollisionMask(99) == 0u, "A52", "an out-of-range layer collides with nothing");
    }

    // ---------------------------------------------------------------- B ----
    void TestWiringGate()
    {
        const std::string go_header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(!go_header.empty(), "B1", "GameObject.h is readable");
        Check(CountOccurrences(go_header, "m_Tag") >= 2, "B2", "GameObject declares and uses m_Tag");
        Check(CountOccurrences(go_header, "m_Layer") >= 2, "B3", "GameObject declares and uses m_Layer");
        Check(CountOccurrences(go_header, "CompareTag") >= 1, "B4", "CompareTag is declared");
        Check(CountOccurrences(go_header, "FindGameObjectsWithTag") >= 1,
              "B5", "FindGameObjectsWithTag is declared");
        Check(CountOccurrences(go_header, "TagLayerManager.h") >= 1,
              "B6", "GameObject.h includes the tag/layer manager");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "B7", "GameObject.cpp is readable");
        // >= 2: declared in the header *and* written in Transfer(). A field that
        // is only declared is never persisted, which looks exactly like "the
        // editor forgot to save" and costs an afternoon.
        Check(CountOccurrences(go_source, "m_Tag") >= 3, "B8", "m_Tag is transferred (not just declared)");
        Check(CountOccurrences(go_source, "m_Layer") >= 3, "B9", "m_Layer is transferred (not just declared)");
        Check(CountOccurrences(go_source, "TagLayerManager") >= 2,
              "B10", "GameObject.cpp consults the tag registry");
        Check(CountOccurrences(go_source, "IsActiveInHierarchy") >= 1,
              "B11", "tag search filters on activeInHierarchy like Unity");

        const std::string reg = ReadFile("Source/Runtime/RegisterRuntime.cpp");
        Check(!reg.empty(), "B12", "RegisterRuntime.cpp is readable");
        Check(CountOccurrences(reg, "TagLayerManager") >= 2,
              "B13", "TagLayerManager is included AND registered");
        Check(CountOccurrences(reg, "REGISTER_SYSTEM(TagLayerManager)") >= 1,
              "B14", "TagLayerManager is registered as a system");
    }
}  // namespace

int main()
{
    // ctest redirects stdout, which makes it fully buffered: a crash or a hang
    // would then swallow every line printed before it. Unbuffered keeps the log
    // honest.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("=== Unity parity smoke test ===\n\n");

    TagLayerManager manager;
    Check(manager.Initialize(), "A0", "TagLayerManager initializes");

    TestTags(manager);
    TestLayers(manager);
    TestMasks();
    TestCollisionMatrix(manager);
    TestWiringGate();

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return (g_Failures == 0) ? 0 : 1;
}
