#pragma once

#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderType.h"

class EditorTranslationAxis : public RenderEntity
{
public:
    EditorTranslationAxis();
    RenderMeshData m_MeshData;
};

class EditorRotationAxis : public RenderEntity
{
public:
    EditorRotationAxis();
    RenderMeshData m_MeshData;
};

class EditorScaleAxis : public RenderEntity
{
public:
    EditorScaleAxis();
    RenderMeshData m_MeshData;
};
