#include "Runtime/Function/Animation/Node.h"

#include "Runtime/Core/Math/Math.h"

//-----------------------------------------------------------------------
Node::Node(const std::string name)
{
    m_Name = name;
}
Node::~Node()
{
    // clear();
}
//-----------------------------------------------------------------------
void Node::clear() {}
//-----------------------------------------------------------------------
Node* Node::GetParent(void) const
{
    return m_Parent;
}

//-----------------------------------------------------------------------
void Node::SetParent(Node* parent)
{
    m_Parent = parent;
    SetDirty();
}
//-----------------------------------------------------------------------
void Node::Update()
{
    // Update transforms from parent
    UpdateDerivedTransform();
    m_IsDirty = false;
}
//-----------------------------------------------------------------------
void Node::UpdateDerivedTransform(void)
{
    if (m_Parent)
    {
        // Update orientation
        const Quaternion& parentOrientation = m_Parent->_getDerivedOrientation();
        {
            // Combine orientation with that of parent
            m_DerivedOrientation = parentOrientation * m_Orientation;
            m_DerivedOrientation.normalise();
        }

        // Update scale
        const Vector3& parentScale = m_Parent->_getDerivedScale();
        {
            // Scale own position by parent scale, NB just combine
            // as equivalent axes, no shearing
            m_DerivedScale = parentScale * m_Scale;
        }

        // Change position vector based on parent's orientation & scale
        m_DerivedPosition = parentOrientation * (parentScale * m_Position);

        // Add altered position vector to parents
        m_DerivedPosition = m_DerivedPosition + m_Parent->_getDerivedPosition();
    }
    else
    {
        // Root node, no parent
        m_DerivedOrientation = m_Orientation;
        m_DerivedPosition = m_Position;
        m_DerivedScale = m_Scale;
    }
}

//-----------------------------------------------------------------------
const Quaternion& Node::GetOrientation() const
{
    return m_Orientation;
}

//-----------------------------------------------------------------------
void Node::SetOrientation(const Quaternion& q)
{
    // ASSERT(!q.isNaN() && "Invalid orientation supplied as parameter");
    if (q.isNaN())
    {
        // LOG_ERROR(__FUNCTION__, "Invalid orientation supplied as parameter");
        m_Orientation = Quaternion::IDENTITY;
    }
    else
    {
        m_Orientation = q;
        m_Orientation.normalise();
    }
    SetDirty();
}
//-----------------------------------------------------------------------
void Node::ResetOrientation(void)
{
    m_Orientation = Quaternion::IDENTITY;
    SetDirty();
}

//-----------------------------------------------------------------------
void Node::SetPosition(const Vector3& pos)
{
    if (pos.isNaN())
    {
        // LOG_ERROR
    }
    m_Position = pos;
    SetDirty();
}

//-----------------------------------------------------------------------
const Vector3& Node::GetPosition(void) const
{
    return m_Position;
}
//-----------------------------------------------------------------------
void Node::Translate(const Vector3& d, TransformSpace relativeTo)
{
    switch (relativeTo)
    {
        case TransformSpace::LOCAL:
            // position is relative to parent so transform downwards
            m_Position = m_Position + m_Orientation * d;
            break;
        case TransformSpace::OBJECT:
            // position is relative to parent so transform upwards
            if (m_Parent)
            {
                m_Position =
                    m_Position + (m_Parent->_getDerivedOrientation().inverse() * d) / m_Parent->_getDerivedScale();
            }
            else
            {
                m_Position = m_Position + d;
            }
            break;
        case TransformSpace::AREN:
            m_Position = m_Position + d;
            break;
    }
    SetDirty();
}

//-----------------------------------------------------------------------
void Node::Rotate(const Quaternion& q, TransformSpace relativeTo)
{
    // Normalize Quaternionernion to avoid drift
    Quaternion qnorm = q;
    qnorm.normalise();

    switch (relativeTo)
    {
        case TransformSpace::AREN:
            // Rotations are normally relative to local axes, transform up
            m_Orientation = qnorm * m_Orientation;
            break;
        case TransformSpace::OBJECT:
            // Rotations are normally relative to local axes, transform up
            m_Orientation = m_Orientation * _getDerivedOrientation().inverse() * qnorm * _getDerivedOrientation();
            break;
        case TransformSpace::LOCAL:
            // Note the order of the mult, i.e. q comes after
            m_Orientation = m_Orientation * qnorm;
            break;
    }
    SetDirty();
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
const Quaternion& Node::_getDerivedOrientation(void) const
{
    return m_DerivedOrientation;
}
//-----------------------------------------------------------------------
const Vector3& Node::_getDerivedPosition(void) const
{
    return m_DerivedPosition;
}
//-----------------------------------------------------------------------
const Vector3& Node::_getDerivedScale(void) const
{
    return m_DerivedScale;
}

const Matrix4x4& Node::_getInverseTpose(void) const
{
    return m_InverseTpose;
}

//-----------------------------------------------------------------------
void Node::SetScale(const Vector3& inScale)
{
    if (inScale.isNaN())
    {
        // LOG_ERROR
    }
    m_Scale = inScale;
    SetDirty();
}
//-----------------------------------------------------------------------
const Vector3& Node::GetScale(void) const
{
    return m_Scale;
}
//-----------------------------------------------------------------------
void Node::Scale(const Vector3& inScale)
{
    m_Scale = m_Scale * inScale;
    SetDirty();
}
//-----------------------------------------------------------------------
const std::string& Node::GetName(void) const
{
    return m_Name;
}
//-----------------------------------------------------------------------
void Node::SetAsInitialPose(void)
{
    m_InitialPosition = m_Position;
    m_InitialOrientation = m_Orientation;
    m_InitialScale = m_Scale;
}
//-----------------------------------------------------------------------
void Node::ResetToInitialPose(void)
{
    // m_Position = {};// m_InitialPosition;
    // m_Orientation = { {},0,0,0,1 };// m_InitialOrientation;
    // m_Scale = { {},1,1,1 };//m_InitialScale;
    m_Position = m_InitialPosition;
    m_Orientation = m_InitialOrientation;
    m_Scale = m_InitialScale;
    SetDirty();
}
//-----------------------------------------------------------------------
const Vector3& Node::GetInitialPosition(void) const
{
    return m_InitialPosition;
}
//-----------------------------------------------------------------------
const Quaternion& Node::GetInitialOrientation(void) const
{
    return m_InitialOrientation;
}
//-----------------------------------------------------------------------
const Vector3& Node::GetInitialScale(void) const
{
    return m_InitialScale;
}
//-----------------------------------------------------------------------
void Node::SetDirty()
{
    m_IsDirty = true;
}

bool Node::IsDirty() const
{
    return m_IsDirty;
}
//---------------------------------------------------------------------
Bone::Bone()
    : Node(std::string()) {}

void Bone::Initialize(RawBone* definition, Bone* parent_bone)
{
    m_Definition = definition;

    if (definition)
    {
        m_Name = definition->name;
        SetOrientation(definition->binding_pose.m_Rotation);
        SetPosition(definition->binding_pose.m_Position.ToVector3());
        SetScale(definition->binding_pose.m_Scale);
        m_InverseTpose = definition->tpose_matrix;
        SetAsInitialPose();
    }
    m_Parent = parent_bone;
}

//---------------------------------------------------------------------
size_t Bone::GetID(void) const
{
    if (m_Definition)
    {
        return m_Definition->index;
    }
    return std::numeric_limits<size_t>().max();
}