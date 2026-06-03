#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Camera.h"

#include <type_traits>

class RenderCamera;

enum class CameraMode : unsigned char
{
    third_person,
    first_person,
    free,
    invalid
};

template<>
class SerializeTraits<CameraMode> : public SerializeTraitsBase<CameraMode>
{
public:
    using value_type = CameraMode;

    inline static const char* GetTypeString(void* p = nullptr) { return "CameraMode"; }
    inline static bool AllowTransferOptimization() { return true; }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        using UnderlyingType = std::underlying_type_t<value_type>;
        transfer.TransferBasicData(reinterpret_cast<UnderlyingType&>(data));
    }
};

class CameraComponent : public Component
{
    REGISTER_CLASS(CameraComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    CameraComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;

    void Tick(float delta_time) override;

    void Initialize(CameraMode mode, CameraParameter* parameter);

    CameraMode getCameraMode() const { return m_CameraMode; }
    void setCameraMode(CameraMode mode) { m_CameraMode = mode; }
    bool isMainCamera() const { return m_IsMainCamera; }
    void SetMainCamera(bool is_main_camera) { m_IsMainCamera = is_main_camera; }
    CameraParameter* GetCameraParameter() const;
    Vector3 GetPosition() const { return m_Position; }

    Vector3 getForward() const { return m_Forward; }
    bool ApplyRenderTexture();
    void ApplyToGameRenderCamera(RenderCamera& render_camera) const;

private:
    void TickFirstPersonCamera(float delta_time);
    void TickThirdPersonCamera(float delta_time);
    void TickFreeCamera(float delta_time);

    CameraComponentRes m_CameraRes;

    CameraMode m_CameraMode {CameraMode::invalid};
    bool m_IsMainCamera {false};

    Vector3 m_Position;

    Vector3 m_Forward {Vector3::NEGATIVE_UNIT_Y};
    Vector3 m_Up {Vector3::UNIT_Z};
    Vector3 m_Left {Vector3::UNIT_X};
};