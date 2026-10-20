#pragma once

#include "RenderTextureDesc.h"
#include "Texture.h"
#include "RenderTexture.generated.h"

// Not serialized yet (m_Desc is runtime-only); registered so that
// PPtr<RenderTexture> (RenderCamera::targetTexture) resolves to a real Type.
ZCLASS()
class RenderTexture : public Texture
{
    ZGENERATED_BODY(RenderTexture)

public:
    int GetWidth() const { return m_Desc.width; }
    int GetHeight() const { return m_Desc.height; }

private:
    RenderTextureDesc m_Desc;
};