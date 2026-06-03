#include "GlobalRendering.h"

IMPLEMENT_REGISTER_CLASS(GlobalRenderingRes)
IMPLEMENT_OBJECT_SERIALIZE(GlobalRenderingRes)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(GlobalRenderingRes)

template<typename TransferFunction>
void GlobalRenderingRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_EnableFxaa, "enable_fxaa");
    transfer.Transfer(m_SkyboxIrradianceMap, "skybox_irradiance_map");
    transfer.Transfer(m_SkyboxSpecularMap, "skybox_specular_map");
    transfer.Transfer(m_BrdfMap, "brdf_map");
    transfer.Transfer(m_ColorGradingMap, "color_grading_map");

    transfer.Transfer(m_SkyColor, "sky_color");
    transfer.Transfer(m_AmbientLight, "ambient_light");
    transfer.Transfer(m_CameraConfig, "camera_config");
    transfer.Transfer(m_DirectionalLight, "directional_light");
}

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