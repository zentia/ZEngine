#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "LightComponent.generated.h"

ZCLASS()
class LightComponent : public Component
{
    ZGENERATED_BODY(LightComponent)

public:
    ZFUNCTION()
    void SetFlux(const Vector3& flux) { m_Flux = flux; }
    ZFUNCTION()
    Vector3 GetFlux() const { return m_Flux; }

    ZFUNCTION()
    void SetRadius(float radius) { m_Radius = radius; }
    ZFUNCTION()
    float GetRadius() const { return m_Radius; }

private:
    ZPROPERTY(meta=(Tooltip="Radiant flux per RGB channel."))
    Vector3 m_Flux {Vector3(100.0f, 100.0f, 100.0f)};
    ZPROPERTY(meta=(Tooltip="Attenuation radius in world units; the light has no effect beyond it.", ClampMin=0))
    float m_Radius {8.0f};
};
