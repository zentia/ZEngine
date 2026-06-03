#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"

#include <vector>

class Character
{
    inline static const float s_CameraBlendTime {0.3f};

public:
    Character(std::shared_ptr<GameObject> character_object);

    GObjectID GetObjectID() const;
    void SetObject(std::shared_ptr<GameObject> gobject);
    std::weak_ptr<GameObject> getObject() const { return m_CharacterObject; }

    void SetPosition(const Vector3& position) { m_Position = position; }
    void SetRotation(const Quaternion& rotation) { m_Rotation = rotation; }

    const Vector3& GetPosition() const { return m_Position; }
    const Quaternion& getRotation() const { return m_Rotation; }

    void Tick(float delta_time);

private:
    void ToggleFreeCamera();

    Vector3 m_Position;
    Quaternion m_Rotation;

    std::shared_ptr<GameObject> m_CharacterObject;

    // hack for setting rotation frame buffer
    Quaternion m_RotationBuffer;
    bool m_RotationDirty {false};

    CameraMode m_OriginalCameraMode;
    bool m_IsFreeCamera {false};
};