#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Camera.h"

#include <type_traits>

#include "CameraComponent.generated.h"

class RenderCamera;

// Scripts see this as `declare enum CameraMode { ThirdPerson = 0, ... }`, and
// the runtime publishes a global object of the same shape, so
// `camera.mode = CameraMode.Free` both type-checks and evaluates.
ZENUM()
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

ZCLASS()
class CameraComponent : public Component
{
    ZGENERATED_BODY(CameraComponent)

public:
    CameraComponent() = default;
    /// Frees the DEFAULT camera parameter this component created (and only that one:
    /// a parameter passed to Initialize() or read from a file belongs to its creator).
    ~CameraComponent() override;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;

    void Tick(float delta_time) override;

    void Initialize(CameraMode mode, CameraParameter* parameter);

    // CameraMode is a ZENUM, so it crosses as an int32 and the .d.ts declares
    // it as `CameraMode` -- scripts write `setCameraMode(CameraMode.Free)`.
    ZFUNCTION()
    CameraMode getCameraMode() const { return m_CameraMode; }
    ZFUNCTION()
    void setCameraMode(CameraMode mode) { m_CameraMode = mode; }

    /// Mutable access for importers/migrators. The camera parameter is stored
    /// inline in legacy JSON but held by PPtr at runtime, so it has to be
    /// patched in after the component's own Transfer has run.
    CameraComponentRes& GetCameraResMutable() { return m_CameraRes; }

    ZFUNCTION()
    bool isMainCamera() const { return m_IsMainCamera; }
    ZFUNCTION()
    void SetMainCamera(bool is_main_camera) { m_IsMainCamera = is_main_camera; }
    CameraParameter* GetCameraParameter() const;
    ZFUNCTION()
    Vector3 GetPosition() const { return m_Position; }

    ZFUNCTION()
    Vector3 getForward() const { return m_Forward; }
    bool ApplyRenderTexture();
    void ApplyToGameRenderCamera(RenderCamera& render_camera) const;

private:
    void TickFirstPersonCamera(float delta_time);
    void TickThirdPersonCamera(float delta_time);
    void TickFreeCamera(float delta_time);

    // Serialized order is mode, main, settings (kept from the hand-written
    // Transfer so existing .zasset / StreamedBinary payloads stay valid).
    ZPROPERTY(Name="mode")
    CameraMode m_CameraMode {CameraMode::invalid};
    ZPROPERTY(Name="main")
    bool m_IsMainCamera {false};
    ZPROPERTY(Name="settings")
    CameraComponentRes m_CameraRes;

    /// Instance ID of the default CameraParameter this component created itself, or 0 when
    /// the current parameter came from elsewhere. Held as an ID, not a pointer, on purpose:
    /// it is compared against the live parameter's ID, so a parameter that was replaced
    /// from outside (Initialize / a field edit) is never freed by mistake. NOT serialized.
    int32_t m_OwnedDefaultParameterId {0};

    /// Create a default parameter for `mode` through ObjectManager::NewObject (real instance
    /// ID, so the PPtr resolves) and remember it as owned. Frees a previously owned one.
    void CreateOwnedDefaultParameter();
    /// Destroy the owned default parameter, if any (looked up by the remembered ID, so it is
    /// freed even after something else replaced it as the current parameter).
    void ReleaseOwnedDefaultParameter();
    /// A CLONE (prefab instantiate, copy/paste) comes out of a serialization round trip with
    /// the original's parameter ID, i.e. pointing at a default the ORIGINAL owns and will free.
    /// If this camera's parameter is owned by another camera, replace it with a private copy.
    void DetachFromParameterOwnedElsewhere();

    Vector3 m_Position;

    Vector3 m_Forward {Vector3::NEGATIVE_UNIT_Y};
    Vector3 m_Up {Vector3::UNIT_Z};
    Vector3 m_Left {Vector3::UNIT_X};
};
