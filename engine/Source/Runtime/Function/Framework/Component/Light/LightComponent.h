#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <cstdint>

#include "LightComponent.generated.h"

ZCLASS()
class LightComponent : public Component
{
    ZGENERATED_BODY(LightComponent)

public:
    // Light type. Kept as uint8_t (not an enum) so it round-trips through the
    // reflection/Inspector property bag without needing enum registration; the
    // shader side also keys the spot branch off a float, so the mapping stays
    // trivial. 0 = Point (default, unchanged behaviour), 1 = Spot.
    static constexpr uint8_t kTypePoint = 0;
    static constexpr uint8_t kTypeSpot  = 1;

    ZFUNCTION()
    void SetFlux(const Vector3& flux) { m_Flux = flux; }
    ZFUNCTION()
    Vector3 GetFlux() const { return m_Flux; }

    ZFUNCTION()
    void SetRadius(float radius) { m_Radius = radius; }
    ZFUNCTION()
    float GetRadius() const { return m_Radius; }

    ZFUNCTION()
    void SetType(uint8_t type) { m_Type = type; }
    ZFUNCTION()
    uint8_t GetType() const { return m_Type; }
    ZFUNCTION()
    bool IsSpot() const { return m_Type == kTypeSpot; }

    ZFUNCTION()
    void SetDirection(const Vector3& direction) { m_Direction = direction; }
    ZFUNCTION()
    Vector3 GetDirection() const { return m_Direction; }

    ZFUNCTION()
    void SetInnerConeAngle(float radians) { m_InnerConeAngle = radians; }
    ZFUNCTION()
    float GetInnerConeAngle() const { return m_InnerConeAngle; }

    ZFUNCTION()
    void SetOuterConeAngle(float radians) { m_OuterConeAngle = radians; }
    ZFUNCTION()
    float GetOuterConeAngle() const { return m_OuterConeAngle; }

private:
    ZPROPERTY(meta=(Tooltip="Radiant flux per RGB channel."))
    Vector3 m_Flux {Vector3(100.0f, 100.0f, 100.0f)};
    ZPROPERTY(meta=(Tooltip="Attenuation radius in world units; the light has no effect beyond it.", ClampMin=0))
    float m_Radius {8.0f};

    // ---- Spot light only (ignored when m_Type == kTypePoint) -----------------
    ZPROPERTY(meta=(Tooltip="Light type: 0 = Point, 1 = Spot.", ClampMin=0, ClampMax=1))
    uint8_t m_Type {kTypePoint};
    ZPROPERTY(meta=(Tooltip="Spot forward direction (world space; normalised at upload)."))
    Vector3 m_Direction {Vector3(0.0f, 0.0f, -1.0f)};
    ZPROPERTY(meta=(Tooltip="Inner cone half-angle in radians (full brightness inside).", ClampMin=0, ClampMax=1.5708))
    float m_InnerConeAngle {0.4363f};   // ~25 degrees
    ZPROPERTY(meta=(Tooltip="Outer cone half-angle in radians (zero at/beyond this).", ClampMin=0, ClampMax=1.5708))
    float m_OuterConeAngle {0.7854f};   // ~45 degrees
};
