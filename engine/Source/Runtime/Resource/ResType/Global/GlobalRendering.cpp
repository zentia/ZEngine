#include "GlobalRendering.h"

#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"


template<typename TransferFunction>
void SkyBoxIrradianceMap::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_NegativeXMap, "negative_x_map");
    transfer.Transfer(m_PositiveXMap, "positive_x_map");
    transfer.Transfer(m_NegativeYMap, "negative_y_map");
    transfer.Transfer(m_PositiveYMap, "positive_y_map");
    transfer.Transfer(m_NegativeZMap, "negative_z_map");
    transfer.Transfer(m_PositiveZMap, "positive_z_map");
}

template<typename TransferFunction>
void SkyBoxSpecularMap::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_NegativeXMap, "negative_x_map");
    transfer.Transfer(m_PositiveXMap, "positive_x_map");
    transfer.Transfer(m_NegativeYMap, "negative_y_map");
    transfer.Transfer(m_PositiveYMap, "positive_y_map");
    transfer.Transfer(m_NegativeZMap, "negative_z_map");
    transfer.Transfer(m_PositiveZMap, "positive_z_map");
}

template<typename TransferFunction>
void DirectionalLight::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Direction, "direction");
    transfer.Transfer(m_Color, "color");
}

// The owning ZCLASS's Transfer() is generated into a separate .gen.cpp, so the
// helper structs' Transfer() templates defined here are no longer instantiated
// implicitly by this TU -- instantiate them explicitly.
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(SkyBoxIrradianceMap, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(SkyBoxSpecularMap, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(DirectionalLight, , Transfer, void)
