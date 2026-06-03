#include "Runtime/Core/YamlSerialize/YamlObjectGraph.h"

#include "Runtime/BaseClasses/IPPtrResolver.h"
#include "Runtime/BaseClasses/LocalSerializedObjectIdentifier.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/JsonSerialize/JSONAllocator.h"
#include "Runtime/Core/Serialize/SerializedFile.h"  // FileIdentifier
#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"
#include "Runtime/Core/YamlSerialize/YamlText.h"

#include "rapidjson/document.h"

#include <string>
#include <unordered_map>

namespace
{
    using GraphValue = rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator>;
    using GraphDocument = rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator>;

    // Self-contained IPPtrResolver for a single graph write/read session.
    // Mirrors SerializedFilePPtrResolver but owns its own externals vector so
    // we don't have to stand up a (binary-only, partially-initialized)
    // SerializedFile just to host the externals table.
    class YamlGraphResolver : public IPPtrResolver
    {
    public:
        explicit YamlGraphResolver(ZYaml::GraphWriterExternHook writerHook = {},
                                   ZYaml::GraphReaderExternHook readerHook = {})
            : m_WriterHook(std::move(writerHook)), m_ReaderHook(std::move(readerHook))
        {
        }

        void RegisterLocalObject(int32_t instanceID, int64_t pathID)
        {
            if (instanceID == 0 || pathID == 0)
                return;
            m_LocalInstanceToPath[instanceID] = pathID;
            m_LocalPathToInstance[pathID] = instanceID;
        }

        // Reader side: seed the externals table from the parsed document, in the
        // same order it was written so 1-based m_FileID indices line up.
        int32_t AddExternalForRead(const FileIdentifier& ref)
        {
            m_Externals.push_back(ref);
            return static_cast<int32_t>(m_Externals.size());
        }

        const std::vector<FileIdentifier>& GetExternals() const { return m_Externals; }

        int32_t LSOIToInstanceID(const LocalSerializedObjectIdentifier& lsoi) override
        {
            if (lsoi.localSerializedFileIndex == 0 && lsoi.localIdentifierInFile == 0)
                return 0;

            if (lsoi.localSerializedFileIndex == 0)
            {
                auto it = m_LocalPathToInstance.find(lsoi.localIdentifierInFile);
                return (it != m_LocalPathToInstance.end()) ? it->second : 0;
            }

            if (!m_ReaderHook)
                return 0;

            const int32_t idx = lsoi.localSerializedFileIndex - 1;
            if (idx < 0 || static_cast<size_t>(idx) >= m_Externals.size())
                return 0;

            return m_ReaderHook(m_Externals[idx], lsoi.localIdentifierInFile);
        }

        void InstanceIDToLSOI(int32_t instanceID, LocalSerializedObjectIdentifier& out) override
        {
            out.localSerializedFileIndex = 0;
            out.localIdentifierInFile = 0;
            if (instanceID == 0)
                return;

            auto localIt = m_LocalInstanceToPath.find(instanceID);
            if (localIt != m_LocalInstanceToPath.end())
            {
                out.localIdentifierInFile = localIt->second;
                return;
            }

            if (!m_WriterHook)
                return;

            FileIdentifier ref;
            int64_t pathID = 0;
            if (!m_WriterHook(instanceID, ref, pathID))
                return;

            out.localSerializedFileIndex = AddExternalDedup(ref);
            out.localIdentifierInFile = pathID;
        }

    private:
        // Writer-side dedup: same key (guid, else path) returns the existing
        // 1-based index. Matches SerializedFile::AddExternalRef semantics.
        int32_t AddExternalDedup(const FileIdentifier& ref)
        {
            const std::string& key = !ref.guid.empty() ? ref.guid : ref.pathName;
            for (size_t i = 0; i < m_Externals.size(); ++i)
            {
                const FileIdentifier& existing = m_Externals[i];
                const std::string& existingKey = !existing.guid.empty() ? existing.guid : existing.pathName;
                if (!key.empty() && existingKey == key)
                    return static_cast<int32_t>(i + 1);
            }
            m_Externals.push_back(ref);
            return static_cast<int32_t>(m_Externals.size());
        }

        ZYaml::GraphWriterExternHook m_WriterHook;
        ZYaml::GraphReaderExternHook m_ReaderHook;
        std::vector<FileIdentifier> m_Externals;
        std::unordered_map<int32_t, int64_t> m_LocalInstanceToPath;
        std::unordered_map<int64_t, int32_t> m_LocalPathToInstance;
    };

    GraphValue MakeString(const char* s, JSONAllocator& alloc)
    {
        GraphValue v;
        v.SetString(s != nullptr ? s : "", alloc);
        return v;
    }

    int64_t ReadIntField(const GraphValue& v)
    {
        if (v.IsInt64())
            return v.GetInt64();
        if (v.IsInt())
            return v.GetInt();
        if (v.IsUint())
            return v.GetUint();
        if (v.IsUint64())
            return static_cast<int64_t>(v.GetUint64());
        if (v.IsDouble())
            return static_cast<int64_t>(v.GetDouble());
        return 0;
    }
}  // namespace

namespace ZYaml
{
    bool WriteObjectGraph(const std::vector<ObjectGraphEntry>& objects,
                          eastl::string& out,
                          GraphWriterExternHook writerHook)
    {
        YamlGraphResolver resolver(std::move(writerHook), {});

        // Ensure every object has a stable InstanceID and register it as local so
        // PPtr / ImmediatePtr fields targeting siblings encode as (fileID 0, path).
        for (const ObjectGraphEntry& e : objects)
        {
            if (e.object == nullptr)
                continue;
            GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(e.object);
            resolver.RegisterLocalObject(e.object->GetInstanceID(), e.fileID);
        }

        JSONAllocator allocator;
        GraphDocument master(&allocator, 1024, &allocator);
        master.SetObject();
        JSONAllocator& alloc = master.GetAllocator();

        GraphValue objectsArr(rapidjson::kArrayType);
        {
            ScopedPPtrResolver scope(&resolver);
            for (const ObjectGraphEntry& e : objects)
            {
                if (e.object == nullptr)
                    continue;

                YAMLWrite writer(kNoTransferInstructionFlags);
                e.object->VirtualRedirectTransfer(writer);

                GraphValue entry(rapidjson::kObjectType);
                entry.AddMember("fileID", static_cast<int64_t>(e.fileID), alloc);
                entry.AddMember("type", MakeString(e.object->GetTypeName(), alloc), alloc);

                GraphValue data;
                data.CopyFrom(writer.GetDocument(), alloc);
                entry.AddMember("data", data, alloc);

                objectsArr.PushBack(entry, alloc);
            }
        }

        // Externals discovered during the object writes above.
        GraphValue externalsArr(rapidjson::kArrayType);
        for (const FileIdentifier& ref : resolver.GetExternals())
        {
            GraphValue r(rapidjson::kObjectType);
            r.AddMember("guid", MakeString(ref.guid.c_str(), alloc), alloc);
            r.AddMember("type", MakeString(ref.type.c_str(), alloc), alloc);
            r.AddMember("path", MakeString(ref.pathName.c_str(), alloc), alloc);
            externalsArr.PushBack(r, alloc);
        }

        master.AddMember("externals", externalsArr, alloc);
        master.AddMember("objects", objectsArr, alloc);

        ZYaml::EmitYaml(master, out);
        return true;
    }

    bool ReadObjectGraph(const char* text,
                         std::vector<ObjectGraphEntry>& out,
                         GraphReaderExternHook readerHook)
    {
        out.clear();

        JSONAllocator allocator;
        GraphDocument master(&allocator, 1024, &allocator);
        if (!ZYaml::ParseYaml(text, master) || !master.IsObject())
            return false;

        YamlGraphResolver resolver({}, std::move(readerHook));

        // Seed externals first so reader-hook indices line up with write order.
        auto extIt = master.FindMember("externals");
        if (extIt != master.MemberEnd() && extIt->value.IsArray())
        {
            for (const GraphValue& r : extIt->value.GetArray())
            {
                if (!r.IsObject())
                    continue;
                FileIdentifier ref;
                if (r.HasMember("guid") && r["guid"].IsString())
                    ref.guid = r["guid"].GetString();
                if (r.HasMember("type") && r["type"].IsString())
                    ref.type = r["type"].GetString();
                if (r.HasMember("path") && r["path"].IsString())
                    ref.pathName = r["path"].GetString();
                resolver.AddExternalForRead(ref);
            }
        }

        auto objIt = master.FindMember("objects");
        if (objIt == master.MemberEnd() || !objIt->value.IsArray())
            return false;

        // First pass: produce + register every object so references (which may
        // point forward in document order) resolve in the read pass below.
        std::vector<const GraphValue*> dataNodes;
        for (const GraphValue& entry : objIt->value.GetArray())
        {
            if (!entry.IsObject())
                continue;

            const int64_t fileID = entry.HasMember("fileID") ? ReadIntField(entry["fileID"]) : 0;
            const char* typeName =
                (entry.HasMember("type") && entry["type"].IsString()) ? entry["type"].GetString() : nullptr;
            if (fileID == 0 || typeName == nullptr)
                continue;

            const Type* type = TypeManager::GetInstance().ClassNameToType(typeName);
            if (type == nullptr)
            {
                LOG_WARNING(ZSerializer, "YamlObjectGraph: unknown class '{}' (fileID {}), skipping", typeName, fileID);
                continue;
            }

            Object* obj = GET_SYSTEM(ObjectManager)->Produce(type, 0);
            if (obj == nullptr)
            {
                LOG_WARNING(ZSerializer, "YamlObjectGraph: failed to Produce '{}' (fileID {})", typeName, fileID);
                continue;
            }
            GET_SYSTEM(ObjectManager)->AllocateAndAssignInstanceID(obj);
            resolver.RegisterLocalObject(obj->GetInstanceID(), fileID);

            out.push_back({fileID, obj});
            dataNodes.push_back(entry.HasMember("data") ? &entry["data"] : nullptr);
        }

        // Second pass: deserialize fields under an active resolver.
        {
            ScopedPPtrResolver scope(&resolver);
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (dataNodes[i] == nullptr)
                    continue;
                YAMLRead reader(*dataNodes[i], kNoTransferInstructionFlags);
                out[i].object->VirtualRedirectTransfer(reader);
            }
        }

        return true;
    }
}  // namespace ZYaml
