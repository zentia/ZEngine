// =====================================================================
// Reflection codegen smoke test (ZHeaderTool phase 1)
// ---------------------------------------------------------------------
// Standalone executable that dumps, for every class name passed on the
// command line (or a built-in default list):
//
//   * the registered Type record   (name / base / size / abstract / factory)
//   * the full TypeTree            (name / type / byte size / byte offset /
//                                   meta flags, depth-first)
//     `--no-offsets` drops the in-memory byte offsets: they are runtime-only
//     (TypeTree::WriteTypeTree does not persist them), so a member reorder
//     that keeps the serialized order changes them without affecting data.
//   * the StreamedBinaryWrite bytes of a default-constructed instance
//     (hex + FNV-1a 64)
//
// Purpose: prove that migrating a class from hand-written
// REGISTER_CLASS / Transfer() to ZHeaderTool-generated code is
// byte-for-byte invisible to serialization. Workflow:
//
//   1. Build + run BEFORE migrating  -> save stdout as golden.
//   2. Migrate the class to ZCLASS / ZPROPERTY.
//   3. Build + run AFTER migrating   -> diff against golden.
//
// If a golden file is supplied via `--golden <path>`, the test performs
// the diff itself and exits non-zero on any mismatch.
//
// `--check-registry` additionally walks the whole runtime type table and
// fails if any type is still the "[UNREGISTERED]" placeholder (a class that
// has REGISTER_CLASS but no IMPLEMENT_REGISTER_CLASS) or if a producible
// type's base chain does not reach Object.
//
// `--check-behaviour-enabled` verifies that Behaviour-derived components
// serialize Component::m_Enabled ("enabled") in binary and YAML, and that
// YAML written before that field existed still loads (field keeps its
// default, every other field survives).
//
// `--check-property-meta` cross-checks every ZHeaderTool-generated
// ZPROPERTY(meta=(...)) table against the live TypeTree of its class: every
// annotated name must be a real serialized field, every EditCondition must
// name a bool field, and ClampMin <= ClampMax. Catches the drift that appears
// when an annotation and the ZPROPERTY list stop agreeing.
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST=ON
//   cmake --build build --target ReflectionCodegenSmokeTest
// =====================================================================

#include "RegisterRuntime.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PropertyMeta.h"
#include "Runtime/BaseClasses/RuntimeTypeArray.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Core/Serialize/TypeTree.h"
#include "Runtime/Core/Serialize/TypeTreeCache.h"
#include "Runtime/Core/YamlSerialize/YAMLUtility.h"
#include "Runtime/Function/AI/AIControllerComponent.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    bool g_PrintOffsets = true;

    const char* const kDefaultClasses[] = {
        "LightComponent",
        "CameraComponent",
    };

    uint64_t Fnv1a64(const std::vector<uint8_t>& bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t b : bytes)
        {
            hash ^= b;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void DumpTypeTreeNode(std::ostringstream& out, const TypeTreeIterator& node, int depth)
    {
        out << std::string(static_cast<size_t>(depth) * 2, ' ') << node.Name().c_str() << " : " << node.Type().c_str()
            << " size=" << node.ByteSize();
        if (g_PrintOffsets)
        {
            out << " offset=";
            if (node.HasByteOffset())
                out << node.ByteOffset();
            else
                out << "-";
        }
        out << " flags=0x" << std::hex << static_cast<uint32_t>(node.MetaFlags()) << std::dec << "\n";

        for (TypeTreeIterator child = node.Children(); !child.IsNull(); child = child.Next())
            DumpTypeTreeNode(out, child, depth + 1);
    }

    bool CheckRegistry()
    {
        const Type* object_type = TypeOf<Object>();
        RuntimeTypeArray& types = TypeManager::GetInstance().GetRuntimeTypes();
        int problems = 0;
        uint32_t object_types = 0;
        uint32_t slots = 0;
        // Scan every slot, not just [0, count): a broken builder can leave
        // `count` wrong while the slots are still populated.
        for (uint32_t i = 0; i < RuntimeTypeArray::MAX_RUNTIME_TYPES; ++i)
        {
            const Type* type = types.types[i];
            if (type == nullptr)
                continue;
            ++slots;
            if (type->GetRuntimeTypeIndex() != i)
            {
                std::fprintf(stderr, "REGISTRY: '%s' sits in slot %u but has typeIndex %u\n", type->GetName(), i, type->GetRuntimeTypeIndex());
                ++problems;
            }
            if (type->GetFactory() == nullptr)
                continue;  // KERNEL_BUILTIN value types (int, float, Vector3, ...)
            ++object_types;
            for (const Type* base = type->base; base != nullptr; base = base->base)
            {
                if (std::strcmp(base->GetName(), "[UNREGISTERED]") == 0)
                {
                    std::fprintf(stderr, "REGISTRY: '%s' derives from an unregistered class (REGISTER_CLASS without IMPLEMENT_REGISTER_CLASS)\n", type->GetName());
                    ++problems;
                    break;
                }
                if (!base->IsBaseOf(type->GetRuntimeTypeIndex()))
                {
                    std::fprintf(stderr, "REGISTRY: %s::IsBaseOf(%s) is false (base range [%u, +%u), derived index %u)\n", base->GetName(), type->GetName(),
                                 base->derivedFromInfo.typeIndex, base->derivedFromInfo.descendantCount, type->GetRuntimeTypeIndex());
                    ++problems;
                    break;
                }
            }
            if (!object_type->IsBaseOf(type->GetRuntimeTypeIndex()))
            {
                std::fprintf(stderr, "REGISTRY: '%s' is not under Object in the type tree\n", type->GetName());
                ++problems;
            }
        }
        if (types.count != slots)
        {
            std::fprintf(stderr, "REGISTRY: RuntimeTypeArray::count is %u but %u slots are populated\n", types.count, slots);
            ++problems;
        }
        std::fprintf(stderr, "REGISTRY: %u slots, %u Object types checked, %d problem(s)\n", slots, object_types, problems);
        return problems == 0;
    }

    template<typename T>
    bool CheckEnabledRoundTrip(const char* name, bool can_set_enabled)
    {
        bool ok = true;
        auto fail = [&](const char* what) {
            std::fprintf(stderr, "ENABLED[%s]: %s\n", name, what);
            ok = false;
        };

        // YAML: the field is written.
        T* source = MemoryManager::CreateObject<T>();
        eastl::string yaml;
        YAMLUtility::SerializeToYAML(*source, yaml);
        const char* key = std::strstr(yaml.c_str(), "enabled:");
        if (key == nullptr)
        {
            fail("YAML output has no 'enabled' key");
            return false;
        }

        // YAML: a written 'false' is read back.
        const size_t line_begin = static_cast<size_t>(key - yaml.c_str());
        size_t line_end = yaml.find('\n', line_begin);
        if (line_end == eastl::string::npos)
            line_end = yaml.size();
        eastl::string yaml_false = yaml;
        yaml_false.replace(line_begin, line_end - line_begin, "enabled: false");
        T* from_false = MemoryManager::CreateObject<T>();
        YAMLUtility::DeserializeFromYAML(yaml_false.c_str(), *from_false);
        if (from_false->isEnabled())
            fail("YAML 'enabled: false' was not read back");

        // YAML written before the field existed: drop the line entirely.
        eastl::string legacy = yaml;
        size_t erase_end = line_end < legacy.size() ? line_end + 1 : line_end;
        legacy.erase(line_begin, erase_end - line_begin);
        T* from_legacy = MemoryManager::CreateObject<T>();
        YAMLUtility::DeserializeFromYAML(legacy.c_str(), *from_legacy);
        if (!from_legacy->isEnabled())
            fail("legacy YAML without 'enabled' did not keep the default (true)");
        eastl::string rewritten;
        YAMLUtility::SerializeToYAML(*from_legacy, rewritten);
        if (rewritten != yaml)
            fail("legacy YAML did not round-trip the remaining fields unchanged");

        // Binary (StreamedBinary): a disabled component stays disabled.
        if (can_set_enabled)
        {
            source->SetEnabled(false);
            std::vector<uint8_t> bytes;
            TransferUtility::WriteObjectToVector(*source, bytes);
            T* from_binary = MemoryManager::CreateObject<T>();
            if (!TransferUtility::ReadObjectFromVector(*from_binary, bytes))
                fail("binary read failed");
            else if (from_binary->isEnabled())
                fail("binary round-trip lost enabled=false");
        }

        std::fprintf(stderr, "ENABLED[%s]: %s\n", name, ok ? "ok" : "FAILED");
        return ok;
    }

    bool CheckBehaviourEnabled()
    {
        bool ok = true;
        ok &= CheckEnabledRoundTrip<TypeScriptComponent>("TypeScriptComponent", true);
        ok &= CheckEnabledRoundTrip<RenderCamera>("RenderCamera", true);
        // AIControllerComponent::SetEnabled drives the motor / blackboard and
        // needs a parent GameObject, so only the YAML checks run for it.
        ok &= CheckEnabledRoundTrip<AIControllerComponent>("AIControllerComponent", false);
        return ok;
    }

    // Depth-first search of a TypeTree by serialized field name.
    bool FindTypeTreeNode(const TypeTreeIterator& node, const char* name, TypeTreeIterator& out)
    {
        for (TypeTreeIterator child = node.Children(); !child.IsNull(); child = child.Next())
        {
            if (child.Name() == name)
            {
                out = child;
                return true;
            }
            if (!child->IsArray() && FindTypeTreeNode(child, name, out))
                return true;
        }
        return false;
    }

    /// Cross-check the generated meta=(...) tables against the live TypeTree.
    bool CheckPropertyMeta()
    {
        int problems = 0;
        uint32_t classes = 0;
        uint32_t properties = 0;

        for (const ZClassPropertyMeta* cls = PropertyMetaRegistry::Classes(); cls != nullptr; cls = cls->next)
        {
            ++classes;
            if (cls->type == nullptr)
            {
                std::fprintf(stderr, "PROPERTY_META: a class table has no Type*\n");
                ++problems;
                continue;
            }
            if (cls->type->isAbstract || cls->type->GetFactory() == nullptr)
                continue;  // cannot instantiate -> no TypeTree to check against

            Object* instance = cls->type->GetFactory()();
            if (instance == nullptr)
                continue;
            instance->InitializeRuntimeTypeInfo();
            TypeTree tree;
            const bool have_tree = GET_SYSTEM(TypeTreeCache)->GetTypeTree(instance, kDontRequireAllMetaFlags, tree);

            for (size_t i = 0; i < cls->count; ++i)
            {
                const ZPropertyMeta& meta = cls->properties[i];
                ++properties;
                const char* owner = cls->type->GetName();
                if (meta.serializedName == nullptr || meta.serializedName[0] == '\0')
                {
                    std::fprintf(stderr, "PROPERTY_META: %s entry %zu has no serializedName\n", owner, i);
                    ++problems;
                    continue;
                }
                for (const char* text : {meta.displayName, meta.tooltip, meta.category})
                {
                    if (text != nullptr && text[0] == '\0')
                    {
                        std::fprintf(stderr, "PROPERTY_META: %s.%s has an empty meta string\n", owner, meta.serializedName);
                        ++problems;
                    }
                }
                if (meta.hasClampMin && meta.hasClampMax && meta.clampMin > meta.clampMax)
                {
                    std::fprintf(stderr, "PROPERTY_META: %s.%s has ClampMin (%g) > ClampMax (%g)\n", owner, meta.serializedName,
                                 meta.clampMin, meta.clampMax);
                    ++problems;
                }
                if (!have_tree)
                    continue;

                TypeTreeIterator node;
                if (!FindTypeTreeNode(tree.Root(), meta.serializedName, node))
                {
                    std::fprintf(stderr, "PROPERTY_META: %s.%s is annotated but not a serialized field\n", owner, meta.serializedName);
                    ++problems;
                    continue;
                }
                if (meta.editCondition != nullptr && meta.editCondition[0] != '\0')
                {
                    const char* condition = meta.editCondition;
                    if (condition[0] == '!')
                        ++condition;
                    TypeTreeIterator gate;
                    if (!FindTypeTreeNode(tree.Root(), condition, gate))
                    {
                        std::fprintf(stderr, "PROPERTY_META: %s.%s EditCondition '%s' is not a serialized field\n", owner,
                                     meta.serializedName, condition);
                        ++problems;
                    }
                    else if (gate.Type() != "bool")
                    {
                        std::fprintf(stderr, "PROPERTY_META: %s.%s EditCondition '%s' is '%s', not bool\n", owner,
                                     meta.serializedName, condition, gate.Type().c_str());
                        ++problems;
                    }
                }
            }
        }
        std::fprintf(stderr, "PROPERTY_META: %u class table(s), %u annotated field(s), %d problem(s)\n", classes, properties,
                     problems);
        return problems == 0;
    }

    bool DumpClass(std::ostringstream& out, const char* class_name)
    {
        out << "=== " << class_name << " ===\n";

        const Type* type = TypeManager::GetInstance().ClassNameToType(class_name);
        if (type == nullptr)
        {
            out << "  <type not registered>\n";
            return false;
        }

        out << "type.name=" << type->GetName() << "\n";
        out << "type.base=" << (type->base != nullptr ? type->base->GetName() : "<none>") << "\n";
        out << "type.size=" << type->size << "\n";
        out << "type.abstract=" << (type->isAbstract ? 1 : 0) << "\n";
        out << "type.has_factory=" << (type->GetFactory() != nullptr ? 1 : 0) << "\n";

        if (type->isAbstract || type->GetFactory() == nullptr)
            return true;

        Object* instance = type->GetFactory()();
        if (instance == nullptr)
        {
            out << "  <factory returned null>\n";
            return false;
        }
        instance->InitializeRuntimeTypeInfo();

        TypeTree tree;
        if (!GET_SYSTEM(TypeTreeCache)->GetTypeTree(instance, kDontRequireAllMetaFlags, tree))
        {
            out << "  <GetTypeTree failed>\n";
            return false;
        }
        out << "typetree:\n";
        DumpTypeTreeNode(out, tree.Root(), 1);

        std::vector<uint8_t> bytes;
        TransferUtility::WriteObjectToVector(*instance, bytes);
        out << "binary.size=" << bytes.size() << "\n";
        out << "binary.fnv1a64=0x" << std::hex << Fnv1a64(bytes) << std::dec << "\n";
        out << "binary.hex=";
        static const char* kHex = "0123456789abcdef";
        for (uint8_t b : bytes)
            out << kHex[b >> 4] << kHex[b & 0xF];
        out << "\n";

        // Intentionally leaked: the factory uses plain `new`, and tearing the
        // object down here would only add noise to a dump-only test.
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> classes;
    std::string golden_path;
    bool check_registry = false;
    bool check_enabled = false;
    bool check_property_meta = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--golden" && i + 1 < argc)
            golden_path = argv[++i];
        else if (arg == "--no-offsets")
            g_PrintOffsets = false;
        else if (arg == "--check-registry")
            check_registry = true;
        else if (arg == "--check-behaviour-enabled")
            check_enabled = true;
        else if (arg == "--check-property-meta")
            check_property_meta = true;
        else
            classes.push_back(arg);
    }
    if (classes.empty())
    {
        for (const char* name : kDefaultClasses)
            classes.emplace_back(name);
    }

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    std::ostringstream out;
    bool ok = true;
    for (const std::string& name : classes)
        ok &= DumpClass(out, name.c_str());

    const std::string dump = out.str();
    std::fwrite(dump.data(), 1, dump.size(), stdout);
    std::fflush(stdout);

    if (!ok)
    {
        std::fprintf(stderr, "FAIL: one or more classes could not be dumped.\n");
        return 1;
    }
    if (check_registry && !CheckRegistry())
    {
        std::fprintf(stderr, "FAIL: type registry check failed.\n");
        return 1;
    }
    if (check_enabled && !CheckBehaviourEnabled())
    {
        std::fprintf(stderr, "FAIL: Behaviour 'enabled' serialization check failed.\n");
        return 1;
    }
    if (check_property_meta && !CheckPropertyMeta())
    {
        std::fprintf(stderr, "FAIL: ZPROPERTY meta check failed.\n");
        return 1;
    }

    if (!golden_path.empty())
    {
        std::ifstream golden_file(golden_path, std::ios::binary);
        if (!golden_file)
        {
            std::fprintf(stderr, "FAIL: cannot open golden file %s\n", golden_path.c_str());
            return 1;
        }
        std::stringstream golden;
        golden << golden_file.rdbuf();
        if (golden.str() != dump)
        {
            std::fprintf(stderr, "FAIL: dump differs from golden %s\n", golden_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "PASS: dump matches golden %s\n", golden_path.c_str());
    }
    return 0;
}
