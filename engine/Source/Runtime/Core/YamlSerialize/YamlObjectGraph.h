#pragma once

// =====================================================================================
// YamlObjectGraph -- a text ("YAML") analogue of SerializedFile's multi-object
// write/read, used to persist scenes (.scene), prefabs (.prefab), and
// materials (.mat) as human-readable, diff-friendly object graphs while
// imported DDC assets (textures / meshes / animations) stay binary .zasset.
//
// On-disk shape (one block-YAML document):
//
//   externals:
//     - guid: "..."          # 0-based; referenced as m_FileID = index + 1
//       type: "MaterialRes"
//       path: "Assets/Foo.zasset"
//   objects:
//     - fileID: 1            # Unity-style local file id (anchor)
//       type: "LevelRes"     # class-name tag -> TypeManager::ClassNameToType
//       data:
//         LevelRes:          # body is exactly what YAMLWrite emits for the object
//           gravity: {x: 0, y: 0, z: -9.8}
//           character_name: "Player"
//     - fileID: 2
//       type: "GameObject"
//       data:
//         GameObject:
//           m_Name: "Player"
//           m_Component:
//             - component: {m_FileID: 0, m_PathID: 3}   # local ref -> fileID 3
//
// Cross-object references go through the SAME per-thread IPPtrResolver that
// PPtr<T> / ImmediatePtr<T> use for the binary path: local targets encode as
// (m_FileID = 0, m_PathID = target fileID); external targets encode as
// (m_FileID = externals index + 1, m_PathID = target local id in that file),
// with the externals table carrying the GUID/type/path.
//
// The two host hooks bridge external references to the asset system (AssetManager
// supplies them, reusing the exact logic from its binary WriteFile / ReadObject).
// They are optional: a graph with only local references round-trips with no hooks.
// =====================================================================================

#include <EASTL/string.h>

#include <cstdint>
#include <functional>
#include <vector>

class Object;
struct FileIdentifier;

namespace ZYaml
{
    struct ObjectGraphEntry
    {
        int64_t fileID = 0;
        Object* object = nullptr;
    };

    // Writer hook: given an InstanceID that is NOT local to this graph, fill the
    // external FileIdentifier (guid/type/path) and the target's local id in that
    // external file. Return false to drop the reference (serializes as null).
    using GraphWriterExternHook =
        std::function<bool(int32_t instanceID, FileIdentifier& outRef, int64_t& outPathID)>;

    // Reader hook: given an external FileIdentifier (recovered from the externals
    // table) and a path id, return the runtime InstanceID (0 => null / unresolved).
    using GraphReaderExternHook =
        std::function<int32_t(const FileIdentifier& ref, int64_t pathID)>;

    // Serialize the given objects (each tagged with a unique fileID) to a YAML
    // string. InstanceIDs are assigned/registered for any object lacking one so
    // local references resolve. Returns false only on a hard internal error.
    bool WriteObjectGraph(const std::vector<ObjectGraphEntry>& objects,
                          eastl::string& out,
                          GraphWriterExternHook writerHook = {});

    // Parse a YAML string produced by WriteObjectGraph: produce each object by
    // class-name tag, register it, then deserialize fields with local + external
    // references resolved. `out` receives (fileID, Object*) for every produced
    // object, in document order. Caller owns the produced objects.
    bool ReadObjectGraph(const char* text,
                         std::vector<ObjectGraphEntry>& out,
                         GraphReaderExternHook readerHook = {});

    // Lightweight type sniff for AssetRegistry / GetAssetTypeName. Returns the
    // `type:` tag of the object with `preferredFileID`, or the first object.
    bool PeekPrimaryObjectType(const char* text, int64_t preferredFileID, eastl::string& outType);

    // Deserialize a single pre-produced object from a YAML graph document. The
    // caller must Produce() `outObject` and register its persistent InstanceID
    // via GetInstanceIDFromPathAndFileID before calling. Only the entry whose
    // fileID matches `targetFileID` is transferred into `outObject`.
    bool ReadSingleObjectFromGraph(const char* text,
                                   int64_t targetFileID,
                                   Object* outObject,
                                   GraphReaderExternHook readerHook = {});
}  // namespace ZYaml
