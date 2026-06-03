#pragma once

#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <array>

static const float k_debug_draw_infinity_life_time = -2.f;
static const float k_debug_draw_one_frame = 0.0f;

enum DebugDrawTimeType : uint8_t
{
    _debugDrawTimeType_infinity,
    _debugDrawTimeType_one_frame,
    _debugDrawTimeType_common
};

enum DebugDrawPrimitiveType : uint8_t
{
    _debug_draw_primitive_type_point = 0,
    _debug_draw_primitive_type_line,
    _debug_draw_primitive_type_triangle,
    _debug_draw_primitive_type_quad,
    _debug_draw_primitive_type_draw_box,
    _debug_draw_primitive_type_cylinder,
    _debug_draw_primitive_type_sphere,
    _debug_draw_primitive_type_capsule,
    _debug_draw_primitive_type_text,
    k_debug_draw_primitive_type_count
};
enum FillMode : uint8_t
{
    _FillMode_wireframe = 0,
    _FillMode_solid = 1,
    k_FillMode_count,
};

struct DebugDrawVertex
{
    Vector3 pos;
    Vector4 color;
    Vector2 texcoord;
    DebugDrawVertex()
    {
        pos = Vector3(-1.0f, -1.0f, -1.0f);
        color = Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
        texcoord = Vector2(-1.0f, -1.0f);
    }

    static std::array<RHIVertexInputBindingDescription, 1> GetBindingDescriptions()
    {
        std::array<RHIVertexInputBindingDescription, 1> binding_descriptions;
        binding_descriptions[0].binding = 0;
        binding_descriptions[0].stride = sizeof(DebugDrawVertex);
        binding_descriptions[0].inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;

        return binding_descriptions;
    }

    static std::array<RHIVertexInputAttributeDescription, 3> GetAttributeDescriptions()
    {
        std::array<RHIVertexInputAttributeDescription, 3> attribute_descriptions {};

        // position
        attribute_descriptions[0].binding = 0;
        attribute_descriptions[0].location = 0;
        attribute_descriptions[0].format = RHI_FORMAT_R32G32B32_SFLOAT;
        attribute_descriptions[0].offset = offsetof(DebugDrawVertex, pos);

        // color
        attribute_descriptions[1].binding = 0;
        attribute_descriptions[1].location = 1;
        attribute_descriptions[1].format = RHI_FORMAT_R32G32B32A32_SFLOAT;
        attribute_descriptions[1].offset = offsetof(DebugDrawVertex, color);

        // texcoord
        attribute_descriptions[2].binding = 0;
        attribute_descriptions[2].location = 2;
        attribute_descriptions[2].format = RHI_FORMAT_R32G32_SFLOAT;
        attribute_descriptions[2].offset = offsetof(DebugDrawVertex, texcoord);

        return attribute_descriptions;
    }
};

class DebugDrawPrimitive
{
public:
    DebugDrawTimeType m_TimeType {_debugDrawTimeType_infinity};
    float m_LifeTime {0.f};
    FillMode m_FillMode {_FillMode_wireframe};
    bool m_NoDepthTest = false;

public:
    bool IsTimeOut(float delta_time);
    void SetTime(float in_life_time);

private:
    bool m_Rendered = false;  // for one frame object
};

class DebugDrawPoint : public DebugDrawPrimitive
{
public:
    DebugDrawVertex m_Vertex;
    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_point;
};

class DebugDrawLine : public DebugDrawPrimitive
{
public:
    DebugDrawVertex m_Vertex[2];
    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_line;
};

class DebugDrawTriangle : public DebugDrawPrimitive
{
public:
    DebugDrawVertex m_Vertex[3];
    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_triangle;
};

class DebugDrawQuad : public DebugDrawPrimitive
{
public:
    DebugDrawVertex m_Vertex[4];

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_quad;
};

class DebugDrawBox : public DebugDrawPrimitive
{
public:
    Vector3 m_CenterPoint;
    Vector3 m_HalfExtents;
    Vector4 m_Color;
    Vector4 m_Rotate;

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_draw_box;
};

class DebugDrawCylinder : public DebugDrawPrimitive
{
public:
    Vector3 m_Center;
    Vector4 m_Rotate;
    float m_Radius {0.f};
    float m_Height {0.f};
    Vector4 m_Color;

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_cylinder;
};
class DebugDrawSphere : public DebugDrawPrimitive
{
public:
    Vector3 m_Center;
    float m_Radius {0.f};
    Vector4 m_Color;

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_sphere;
};

class DebugDrawCapsule : public DebugDrawPrimitive
{
public:
    Vector3 m_Center;
    Vector4 m_Rotation;
    Vector3 m_Scale;
    float m_Radius {0.f};
    float m_Height {0.f};
    Vector4 m_Color;

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_capsule;
};

class DebugDrawText : public DebugDrawPrimitive
{
public:
    std::string m_Content;
    Vector4 m_Color;
    Vector3 m_Coordinate;
    int m_Size;
    bool m_IsScreenText;

    static const DebugDrawPrimitiveType k_type_enum_value = _debug_draw_primitive_type_text;
};