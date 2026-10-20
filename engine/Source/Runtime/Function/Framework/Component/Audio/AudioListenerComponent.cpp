#include "Runtime/Function/Framework/Component/Audio/AudioListenerComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"


namespace Runtime
{

    void AudioListenerComponent::PostLoadResource(GameObject* parent_object)
    {
        m_ParentObject = parent_object;
        PushListenerTransform();
    }

    void AudioListenerComponent::Tick(float delta_time)
    {
        (void)delta_time;
        PushListenerTransform();
    }

    void AudioListenerComponent::PushListenerTransform()
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_ParentObject == nullptr)
        {
            return;
        }

        const Transform* transform = m_ParentObject->tryGetComponent<Transform>("Transform");
        if (transform == nullptr)
        {
            return;
        }

        // Z-up convention, identical to CameraComponent: forward = -Y, up = +Z.
        const Vector3 position = transform->GetPosition();
        const Vector3 forward = transform->GetRotation() * Vector3::NEGATIVE_UNIT_Y;
        const Vector3 up = transform->GetRotation() * Vector3::UNIT_Z;

        audio_system->SetListener(position, forward, up);
    }
}  // namespace Runtime
