#pragma once

#include "Runtime/Function/Framework/Component/Component.h"

namespace Runtime
{
    /// Marks its GameObject as the audio listener. Unity's AudioListener.
    ///
    /// ZEngine has one mixer and therefore exactly one meaningful listener: if
    /// several of these tick in the same frame the last one wins. That matches
    /// the engine's single-camera assumption and keeps the mixer free of a
    /// per-listener split.
    class AudioListenerComponent : public Component
    {
        REGISTER_CLASS(AudioListenerComponent);
        DECLARE_OBJECT_SERIALIZE();

    public:
        AudioListenerComponent() = default;

        void PostLoadResource(GameObject* parent_object) override;
        void Tick(float delta_time) override;

    private:
        void PushListenerTransform();
    };
}  // namespace Runtime
