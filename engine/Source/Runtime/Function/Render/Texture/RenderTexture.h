#pragma once

#include "RenderTextureDesc.h"
#include "Texture.h"

class RenderTexture : public Texture
{
    REGISTER_CLASS(RenderTexture);
    DECLARE_OBJECT_SERIALIZE();
    int GetWidth() const { return m_Desc.width; }
    int GetHeight() const { return m_Desc.height; }

private:
    RenderTextureDesc m_Desc;
};