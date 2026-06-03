#pragma once

#include "Interface/RHI.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <array>

struct MeshVertex
{
    struct Position
    {
        Vector3 position;
    };

    struct VaryingBlending
    {
        Vector3 normal;
        Vector3 tangent;
    };

    struct Varying
    {
        Vector2 texcoord;
    };

    struct JointBinding
    {
        int indices[4];
        Vector4 weights;
    };

    static std::array<RHIVertexInputBindingDescription, 3> GetBindingDescriptions()
    {
        std::array<RHIVertexInputBindingDescription, 3> binding_descriptions {};

        // position
        binding_descriptions[0].binding = 0;
        binding_descriptions[0].stride = sizeof(Position);
        binding_descriptions[0].inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;
        // varying blending
        binding_descriptions[1].binding = 1;
        binding_descriptions[1].stride = sizeof(VaryingBlending);
        binding_descriptions[1].inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;
        // varying
        binding_descriptions[2].binding = 2;
        binding_descriptions[2].stride = sizeof(Varying);
        binding_descriptions[2].inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;
        return binding_descriptions;
    }

    static std::array<RHIVertexInputAttributeDescription, 4> GetAttributeDescriptions()
    {
        std::array<RHIVertexInputAttributeDescription, 4> attribute_descriptions {};

        // position
        attribute_descriptions[0].binding = 0;
        attribute_descriptions[0].location = 0;
        attribute_descriptions[0].format = RHI_FORMAT_R32G32B32_SFLOAT;
        attribute_descriptions[0].offset = offsetof(Position, position);

        // varying blending
        attribute_descriptions[1].binding = 1;
        attribute_descriptions[1].location = 1;
        attribute_descriptions[1].format = RHI_FORMAT_R32G32B32_SFLOAT;
        attribute_descriptions[1].offset = offsetof(VaryingBlending, normal);
        attribute_descriptions[2].binding = 1;
        attribute_descriptions[2].location = 2;
        attribute_descriptions[2].format = RHI_FORMAT_R32G32B32_SFLOAT;
        attribute_descriptions[2].offset = offsetof(VaryingBlending, tangent);

        // varying
        attribute_descriptions[3].binding = 2;
        attribute_descriptions[3].location = 3;
        attribute_descriptions[3].format = RHI_FORMAT_R32G32_SFLOAT;
        attribute_descriptions[3].offset = offsetof(Varying, texcoord);

        return attribute_descriptions;
    }
};
