#include "Runtime/UMG/Asset/UWidgetAsset.h"

// RTTI / serialisation boilerplate. The auto-registrar inside
// IMPLEMENT_REGISTER_CLASS registers UWidgetAsset with TypeManager at static-init
// time, so no manual RegisterRuntime.cpp entry is strictly required -- but
// RegisterRuntime.cpp force-references the type to defend against the static
// library dropping this TU's auto-registrar.
IMPLEMENT_REGISTER_CLASS(UWidgetAsset);
IMPLEMENT_OBJECT_SERAILIZE(UWidgetAsset);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(UWidgetAsset)

template<typename TransferFunction>
void UWidgetAsset::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_SchemaVersion, "schema_version");
    transfer.Transfer(m_Nodes, "nodes");
}
