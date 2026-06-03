#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/RenderType.h"

#include <vector>

struct PointLight
{
    Vector3 m_Position;
    // radiant flux in W
    Vector3 m_Flux;

    // calculate an appropriate radius for light culling
    // a windowing function in the shader will perform a smooth transition to zero
    // this is not physically based and usually artist controlled
    float calculateRadius() const
    {
        // radius = where attenuation would lead to an intensity of 1W/m^2
        const float INTENSITY_CUTOFF = 1.0f;
        const float ATTENTUATION_CUTOFF = 0.05f;
        Vector3 intensity = m_Flux / (4.0f * Math_PI);
        float maxIntensity = Vector3::GetMaxElement(intensity);
        float attenuation = Math::max(INTENSITY_CUTOFF, ATTENTUATION_CUTOFF * maxIntensity) / maxIntensity;
        return 1.0f / sqrtf(attenuation);
    }
};

struct AmbientLight
{
    Vector3 m_Irradiance;
};

struct PDirectionalLight
{
    Vector3 m_Direction;
    Vector3 m_Color;
};

struct LightList
{
    // vertex buffers seem to be aligned to 16 bytes
    struct PointLightVertex
    {
        Vector3 m_Position;
        float m_Padding;
        // radiant intensity in W/sr
        // can be calculated from radiant flux
        Vector3 m_Intensity;
        float m_Radius;
    };
};

class PointLightList : public LightList
{
public:
    void init() {}
    void Shutdown() {}

    // upload changes to GPU
    void Update() {}

    std::vector<PointLight> m_Lights;

    std::shared_ptr<BufferData> m_Buffer;
};