#pragma once

#include "Runtime/Core/Math/Math.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

class Node
{
public:
    // Enumeration denoting the spaces which a transform can be relative to.
    enum class TransformSpace
    {
        /// Transform is relative to the local space
        LOCAL,
        /// Transform is relative to the space of the parent pNode
        AREN,
        /// Transform is relative to object space
        OBJECT
    };
#ifdef _DEBUG

public:
#else

protected:
#endif
    Node* m_Parent {nullptr};

    std::string m_Name;

    /// Stores the orientation/position/scale of the pNode relative to it's parent.
    Quaternion m_Orientation {Quaternion::IDENTITY};
    Vector3 m_Position {Vector3::ZERO};
    Vector3 m_Scale {Vector3::UNIT_SCALE};

    // Cached combined orientation/position/scale.
    Quaternion m_DerivedOrientation {Quaternion::IDENTITY};
    Vector3 m_DerivedPosition {Vector3::ZERO};
    Vector3 m_DerivedScale {Vector3::UNIT_SCALE};

    /// The position/orientation/scale to use as a base for keyframe animation
    Vector3 m_InitialPosition {Vector3::ZERO};
    Quaternion m_InitialOrientation {Quaternion::IDENTITY};
    Vector3 m_InitialScale {Vector3::UNIT_SCALE};

    Matrix4x4 m_InverseTpose;

    bool m_IsDirty {true};

protected:
    /// Only available internally - notification of parent.
    virtual void SetParent(Node* parent);

public:
    Node(const std::string name);
    virtual ~Node();
    void clear();
    const std::string& GetName(void) const;
    virtual Node* GetParent(void) const;

    virtual const Quaternion& GetOrientation() const;

    virtual void SetOrientation(const Quaternion& q);
    virtual void ResetOrientation(void);

    virtual void SetPosition(const Vector3& pos);
    virtual const Vector3& GetPosition(void) const;

    virtual void SetScale(const Vector3& scale);
    virtual const Vector3& GetScale(void) const;

    virtual void Scale(const Vector3& scale);

    // Triggers the pNode to update it's combined transforms.
    virtual void UpdateDerivedTransform(void);

    virtual void Translate(const Vector3& d, TransformSpace relativeTo = TransformSpace::AREN);

    // Rotate the pNode around an aritrary axis using a Quarternion.
    virtual void Rotate(const Quaternion& q, TransformSpace relativeTo = TransformSpace::LOCAL);

    // Gets the orientation of the pNode as derived from all parents.
    virtual const Quaternion& _getDerivedOrientation(void) const;
    virtual const Vector3& _getDerivedPosition(void) const;
    virtual const Vector3& _getDerivedScale(void) const;
    virtual const Matrix4x4& _getInverseTpose(void) const;

    // dirty and update
    virtual bool IsDirty() const;
    virtual void SetDirty();
    virtual void Update();

    virtual void SetAsInitialPose(void);
    virtual void ResetToInitialPose(void);

    virtual const Vector3& GetInitialPosition(void) const;
    virtual const Quaternion& GetInitialOrientation(void) const;
    virtual const Vector3& GetInitialScale(void) const;
};

class Bone : public Node
{
    friend class LoDSkeleton;

protected:
    RawBone* m_Definition {};
    // physics simulation and actor status

public:
    Bone();
    void Initialize(RawBone* definition, Bone* parent_bone);

    size_t GetID(void) const;
};