#include "Editor/Prefab/PrefabInstance.h"

#include "Runtime/Core/JsonSerialize/JSONRead.h"
#include "Runtime/Core/JsonSerialize/JSONWrite.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryWrite.h"
#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"

IMPLEMENT_REGISTER_CLASS(PrefabInstance)
IMPLEMENT_OBJECT_SERIALIZE(PrefabInstance)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(PrefabInstance)

template<class TransferFunction>
void PrefabInstance::Transfer(TransferFunction& transfer)
{
    Object::Transfer(transfer);
    transfer.Transfer(m_SourcePrefab, "m_source_prefab");
    transfer.Transfer(m_RootGameObject, "m_root_game_object");
    transfer.Transfer(m_Modifications, "m_modifications");
    transfer.Transfer(m_CorrespondingSource, "m_corresponding_source");
    transfer.Transfer(m_AddedComponents, "m_added_components");
    transfer.Transfer(m_RemovedComponents, "m_removed_components");
}

// CorrespondingSourceEntry is a plain serialisable struct (DECLARE_SERIALIZE), so it
// only needs explicit template instantiations to emit the four Transfer overloads.
template void PrefabInstance::CorrespondingSourceEntry::Transfer<JSONRead>(JSONRead&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<JSONWrite>(JSONWrite&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<YAMLRead>(YAMLRead&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<YAMLWrite>(YAMLWrite&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<StreamedBinaryRead>(StreamedBinaryRead&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<StreamedBinaryWrite>(StreamedBinaryWrite&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<SafeBinaryRead>(SafeBinaryRead&);
template void PrefabInstance::CorrespondingSourceEntry::Transfer<GenerateTypeTreeTransfer>(GenerateTypeTreeTransfer&);

PPtr<Object> PrefabInstance::FindSourceForInstance(PPtr<Object> instance_object) const
{
    const int32_t needle = instance_object.GetInstanceID();
    if (needle == 0)
    {
        return PPtr<Object>();
    }
    for (const auto& entry : m_CorrespondingSource)
    {
        if (entry.instance_object.GetInstanceID() == needle)
        {
            return entry.source_object;
        }
    }
    return PPtr<Object>();
}

PPtr<Object> PrefabInstance::FindInstanceForSource(PPtr<Object> source_object) const
{
    const int32_t needle = source_object.GetInstanceID();
    if (needle == 0)
    {
        return PPtr<Object>();
    }
    for (const auto& entry : m_CorrespondingSource)
    {
        if (entry.source_object.GetInstanceID() == needle)
        {
            return entry.instance_object;
        }
    }
    return PPtr<Object>();
}
