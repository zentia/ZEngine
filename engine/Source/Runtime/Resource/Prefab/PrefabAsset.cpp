#include "Runtime/Resource/Prefab/PrefabAsset.h"

IMPLEMENT_REGISTER_CLASS(PrefabAsset)
IMPLEMENT_OBJECT_SERIALIZE(PrefabAsset)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(PrefabAsset)

template<typename TransferFunction>
void PrefabAsset::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_SchemaVersion, "m_schema_version");
    transfer.Transfer(m_RootGameObject, "m_root_game_object");
    transfer.Transfer(m_VariantBase, "m_variant_base");
}
