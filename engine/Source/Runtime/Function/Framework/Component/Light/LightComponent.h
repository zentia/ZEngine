#pragma once

#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <algorithm>
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

    // =========================================================================
    // Unity 的 `Light.color` / `Light.intensity` / `Light.range` / `spotAngle`
    // =========================================================================
    //
    // 引擎的物理量是 `m_Flux`（**每通道辐射通量**），Unity 那个面是
    // `color`（归一化 RGB）× `intensity`（标量）。两者是**同一份数据的两种写法**：
    //
    //     flux = color.rgb * intensity
    //
    // 所以拆法必须可逆，否则脚本 `light.color = red` 读回来变成粉色 —— 那种 bug
    // 看上去像"颜色空间不对"，查起来全是冤枉路。取
    //
    //     intensity = max(flux.r, flux.g, flux.b)
    //     color     = flux / intensity
    //
    // 是精确的（一次除法一次乘法，误差在 1e-7，肉眼和序列化都看不出来），并且满足
    // 两条 Unity 的行为：
    //
    //   * 默认 `color = white`：flux 三个通道相等时拆出来正好是 (1,1,1)；
    //   * 改 `intensity` 不改色调。
    //
    // 边界：flux 全 0 时 intensity 是 0，除法无意义 —— 这时 color 报**白**：灯"没
    // 有颜色"在 Unity 里是白 + intensity 0，报黑会让脚本以为这是一盏黑灯而不是一
    // 盏关掉的灯。之后 `SetIntensity` 从白起步，于是"把 intensity 从 0 拧到 100"
    // 得到一盏白灯，符合预期。
    //
    // `color.a` **不参与**：辐射通量只有三个通道，alpha 存了也没人读。恒报 1，
    // 写进来的值被丢掉 —— 明确丢掉比"存着但不起作用"好查。

    ZFUNCTION()
    void SetLightColor(const Color& color)
    {
        const float intensity = GetIntensity();
        m_Flux = Vector3(color.r * intensity, color.g * intensity, color.b * intensity);
    }
    ZFUNCTION()
    Color GetLightColor() const
    {
        const float intensity = GetIntensity();
        Color result;
        if (intensity > 0.0f)
        {
            result.r = m_Flux.x / intensity;
            result.g = m_Flux.y / intensity;
            result.b = m_Flux.z / intensity;
        }
        else
        {
            result.r = 1.0f;
            result.g = 1.0f;
            result.b = 1.0f;
        }
        result.a = 1.0f;
        return result;
    }
    ZSCRIPTPROPERTY(Name = "color", Getter = "GetLightColor", Setter = "SetLightColor")

    ZFUNCTION()
    void SetIntensity(float intensity)
    {
        const Color color = GetLightColor();
        m_Flux = Vector3(color.r * intensity, color.g * intensity, color.b * intensity);
    }
    ZFUNCTION()
    float GetIntensity() const { return std::max({m_Flux.x, m_Flux.y, m_Flux.z, 0.0f}); }
    ZSCRIPTPROPERTY(Name = "intensity", Getter = "GetIntensity", Setter = "SetIntensity")

    /// Unity 的 `light.range`：光能照到多远。本引擎的 `radius`，同一件事。
    ZFUNCTION()
    void SetRange(float range) { m_Radius = range; }
    ZFUNCTION()
    float GetRange() const { return m_Radius; }
    ZSCRIPTPROPERTY(Name = "range", Getter = "GetRange", Setter = "SetRange")

    /// Unity 的 `light.spotAngle`：**全**锥角，单位**度**。
    ///
    /// ⚠️ 本引擎存的是 `m_OuterConeAngle` —— **半**锥角，单位**弧度**。差一个 2
    /// 和一个 57.3，写错时画面"看起来像聚光灯但范围不对"，是最难发现的一类单位
    /// bug，所以换算写死在这一处。
    ZFUNCTION()
    void SetSpotAngle(float degrees) { m_OuterConeAngle = (degrees * 0.5f) / Math_fRad2Deg; }
    ZFUNCTION()
    float GetSpotAngle() const { return 2.0f * m_OuterConeAngle * Math_fRad2Deg; }
    ZSCRIPTPROPERTY(Name = "spotAngle", Getter = "GetSpotAngle", Setter = "SetSpotAngle")

    /// Unity 的 `light.innerSpotAngle`：内锥（全亮区）的**全**角，单位度。
    ZFUNCTION()
    void SetInnerSpotAngle(float degrees) { m_InnerConeAngle = (degrees * 0.5f) / Math_fRad2Deg; }
    ZFUNCTION()
    float GetInnerSpotAngle() const { return 2.0f * m_InnerConeAngle * Math_fRad2Deg; }
    ZSCRIPTPROPERTY(Name = "innerSpotAngle", Getter = "GetInnerSpotAngle", Setter = "SetInnerSpotAngle")

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
