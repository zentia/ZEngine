#pragma once
#include "Runtime/Core/Math/Transform.h"

#include <string>
class RawBone
{
public:
    std::string name;
    int index;
    Transform binding_pose;
    Matrix4x4_ tpose_matrix;
    int parent_index;

    // Default constructor
    RawBone() = default;

    // Constructor with index and parent_index
    RawBone(int index, int parent_index)
        : index(index), parent_index(parent_index) {}

    // Copy constructor - manually copy Transform data members since Transform is non-copyable
    RawBone(const RawBone& other)
        : name(other.name), index(other.index), binding_pose(), tpose_matrix(other.tpose_matrix),
          parent_index(other.parent_index)
    {
        binding_pose.m_Position = other.binding_pose.m_Position;
        binding_pose.m_Scale = other.binding_pose.m_Scale;
        binding_pose.m_Rotation = other.binding_pose.m_Rotation;
    }

    // Move constructor
    RawBone(RawBone&& other) noexcept
        : name(std::move(other.name)), index(other.index), binding_pose(), tpose_matrix(other.tpose_matrix),
          parent_index(other.parent_index)
    {
        binding_pose.m_Position = std::move(other.binding_pose.m_Position);
        binding_pose.m_Scale = std::move(other.binding_pose.m_Scale);
        binding_pose.m_Rotation = std::move(other.binding_pose.m_Rotation);
    }

    // Copy assignment operator
    RawBone& operator=(const RawBone& other)
    {
        if (this != &other)
        {
            name = other.name;
            index = other.index;
            binding_pose.m_Position = other.binding_pose.m_Position;
            binding_pose.m_Scale = other.binding_pose.m_Scale;
            binding_pose.m_Rotation = other.binding_pose.m_Rotation;
            tpose_matrix = other.tpose_matrix;
            parent_index = other.parent_index;
        }
        return *this;
    }

    // Move assignment operator
    RawBone& operator=(RawBone&& other) noexcept
    {
        if (this != &other)
        {
            name = std::move(other.name);
            index = other.index;
            binding_pose.m_Position = std::move(other.binding_pose.m_Position);
            binding_pose.m_Scale = std::move(other.binding_pose.m_Scale);
            binding_pose.m_Rotation = std::move(other.binding_pose.m_Rotation);
            tpose_matrix = other.tpose_matrix;
            parent_index = other.parent_index;
        }
        return *this;
    }
};

class SkeletonData : public Object
{
    REGISTER_CLASS(SkeletonData);
    DECLARE_OBJECT_SERIALIZE();

public:
    std::vector<RawBone> bones_map;
    bool is_flat = false;  //"bone.index" equals index in bones_map
    int root_index;
    bool in_topological_order = false;  // TODO: if not in topological order, we need to topology sort in skeleton
                                        // build process
};