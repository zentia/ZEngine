#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "LightComponent.generated.h"

ZCLASS()
class LightComponent : public Component
{
    ZGENERATED_BODY(LightComponent)

public:
    void SetFlux(const Vector3& flux) { m_Flux = flux; }
    Vector3 GetFlux() const { return m_Flux; }

    void SetRadius(float radius) { m_Radius = radius; }
    float GetRadius() const { return m_Radius; }

private:
    ZPROPERTY()
    Vector3 m_Flux {Vector3(100.0f, 100.0f, 100.0f)};
    ZPROPERTY()
    float m_Radius {8.0f};
};
