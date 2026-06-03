#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/CameraConfig.h"

struct SkyBoxIrradianceMap
{
    DECLARE_SERIALIZE(SkyBoxIrradianceMap)

public:
    eastl::string m_NegativeXMap;
    eastl::string m_PositiveXMap;
    eastl::string m_NegativeYMap;
    eastl::string m_PositiveYMap;
    eastl::string m_NegativeZMap;
    eastl::string m_PositiveZMap;
};

struct SkyBoxSpecularMap
{
    DECLARE_SERIALIZE(SkyBoxIrradianceMap)

public:
    eastl::string m_NegativeXMap;
    eastl::string m_PositiveXMap;
    eastl::string m_NegativeYMap;
    eastl::string m_PositiveYMap;
    eastl::string m_NegativeZMap;
    eastl::string m_PositiveZMap;
};

struct DirectionalLight
{
    DECLARE_SERIALIZE(GlobalRenderingRes)

public:
    Vector3 m_Direction;
    Color m_Color;
};

class GlobalRenderingRes : public Object
{
    REGISTER_CLASS(GlobalRenderingRes);
    DECLARE_OBJECT_SERIALIZE();

public:
    bool m_EnableFxaa {false};
    SkyBoxIrradianceMap m_SkyboxIrradianceMap;
    SkyBoxSpecularMap m_SkyboxSpecularMap;
    eastl::string m_BrdfMap;
    eastl::string m_ColorGradingMap;

    Color m_SkyColor;
    Color m_AmbientLight;
    CameraConfig m_CameraConfig;
    DirectionalLight m_DirectionalLight;
};
