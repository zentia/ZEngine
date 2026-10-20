#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/CameraConfig.h"
#include "GlobalRendering.generated.h"

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

ZCLASS()
class GlobalRenderingRes : public Object
{
    ZGENERATED_BODY(GlobalRenderingRes)

public:
    ZPROPERTY()
    bool m_EnableFxaa {false};
    ZPROPERTY()
    SkyBoxIrradianceMap m_SkyboxIrradianceMap;
    ZPROPERTY()
    SkyBoxSpecularMap m_SkyboxSpecularMap;
    ZPROPERTY()
    eastl::string m_BrdfMap;
    ZPROPERTY()
    eastl::string m_ColorGradingMap;

    ZPROPERTY()
    Color m_SkyColor;
    ZPROPERTY()
    Color m_AmbientLight;
    ZPROPERTY()
    CameraConfig m_CameraConfig;
    ZPROPERTY()
    DirectionalLight m_DirectionalLight;
};
