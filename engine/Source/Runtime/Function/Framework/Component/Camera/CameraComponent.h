#pragma once

#include "Runtime/Core/Math/Frustum.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Camera.h"

#include <type_traits>
#include <vector>

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
    /// 登记进 `GetAllCameraComponents()`（见下面那张表的注释）。
    CameraComponent();
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

    // =========================================================================
    // 视锥（`Renderer.isVisible` 用得上）
    // =========================================================================
    //
    // 为什么这一组活在**组件**上而不是 `Camera` 门面（`ScriptCamera.h`）上：
    // `Camera` 门面包的是 `RenderCamera`，而 RenderCamera 归 RenderSystem 管，
    // headless（工具 / 测试 / 专用服务器）下 `GET_SYSTEM(RenderSystem)` 恒为
    // nullptr，`Camera::GetMain()` 直接返回 null —— 于是"这个物体相机看不看得
    // 见"这种判据在测试里根本没法验。而 `CameraComponent` 是场景里的组件，
    // 建一个 GameObject 挂上去就能用。
    //
    // ⚠️ 代价：这套视锥与**真渲染用的**那台相机可能不是同一台（比如编辑器视口
    // 是 RenderCamera、场景里是 CameraComponent）。所以 `Renderer.isVisible`
    // 的语义在这里是"场景里的相机看不看得见"，不是 Unity 的"有没有被渲染到"。
    // 写在注释里，别以后悄悄改。

    /// 相机在世界空间的位置。有 Transform 时以它为准（组件自己的 `m_Position`
    /// 只在 Tick 里被自由/第三人称相机刷）。
    ZFUNCTION()
    Vector3 GetEyePosition() const;

    /// 垂直视场角，**度**。没有参数对象时退回 50（与 `CameraParameter` 的初值一致）。
    ZFUNCTION()
    float GetFieldOfView() const;

    /// Unity 的 `camera.nearClipPlane` / `farClipPlane`，默认 0.3 / 1000（Unity 同）。
    ///
    /// ⚠️ **刻意不做 ZPROPERTY**：给 `CameraComponent` 追加序列化字段会改它
    /// `Transfer()` 的顺序，而 `CameraComponentRes` 的 `PPtr` 迁移本来就脆（见
    /// `DetachFromParameterOwnedElsewhere`）。等真有了相机编辑器再一起加。
    /// 运行时可写，脚本能改，只是不存盘。
    ZFUNCTION()
    float GetNearClipPlane() const { return m_NearClipPlane; }
    ZFUNCTION()
    void SetNearClipPlane(float near_clip) { m_NearClipPlane = near_clip; }
    ZFUNCTION()
    float GetFarClipPlane() const { return m_FarClipPlane; }
    ZFUNCTION()
    void SetFarClipPlane(float far_clip) { m_FarClipPlane = far_clip; }
    ZSCRIPTPROPERTY(Name="nearClipPlane", Getter="GetNearClipPlane", Setter="SetNearClipPlane")
    ZSCRIPTPROPERTY(Name="farClipPlane", Getter="GetFarClipPlane", Setter="SetFarClipPlane")

    /// 宽高比。拿不到窗口（headless）时退回 16:9 —— 用 1:1 会让"竖屏才该出画"
    /// 的东西在测试里也出画，而 16:9 是绝大多数目标的实际值。
    ZFUNCTION()
    float GetAspect() const;

    /// `projection * view`。⚠️ 深度区间是 **z/w ∈ [0,1]**（见 `Frustum.h`
    /// 文件头和 `ScriptVector.h` 里那条被修掉的注释）。
    ZFUNCTION()
    Matrix4x4 GetWorldToClipMatrix() const;

    /// 这台相机**当前**启用且宿主对象在层级里激活吗。枚举相机时要用它过滤：
    /// 停用的相机不渲染，也就不该让任何东西"可见"。
    bool CanSeeAnything() const;

    // ---- 场景里所有相机 -------------------------------------------------
    //
    // 为什么自己维护一张表：`GameObject::FindObjectsOfType` 要活动关卡，而
    // headless 下 `WorldManager::getCurrentActiveLevel()` 恒为 nullptr（见
    // `TestObjectLifecycle` 的 AM1），于是那条路在测试里永远返回空 —— 测出来的
    // "不可见"其实是"没找到相机"，与"真的看不见"长得一模一样。
    static const std::vector<CameraComponent*>& GetAllCameraComponents();

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

    /// 见 `GetNearClipPlane` 的注释：运行时可写，**不**序列化。
    float m_NearClipPlane {0.3f};
    float m_FarClipPlane {1000.f};
};
