#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

class Geometry : public Object
{
    REGISTER_CLASS(Geometry)

public:
    virtual ~Geometry() {}
};

class Box : public Geometry
{
    REGISTER_CLASS(Box)

public:
    DECLARE_SERIALIZE(Box)

    Vector3 m_HalfExtents {0.5f, 0.5f, 0.5f};
};

class Sphere : public Geometry
{
    REGISTER_CLASS(Sphere)

public:
    DECLARE_SERIALIZE(Sphere)

    ~Sphere() override {}
    float m_Radius {0.5f};
};

class Capsule : public Geometry
{
    REGISTER_CLASS(Capsule)

public:
    DECLARE_SERIALIZE(Capsule)

    ~Capsule() override {}
    float m_Radius {0.3f};
    float m_HalfHeight {0.7f};
};

template<typename TransferFunction>
void Box::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_HalfExtents, "half_extents");
}

template<typename TransferFunction>
void Sphere::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Radius, "radius");
}

template<typename TransferFunction>
void Capsule::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Radius, "radius");
    transfer.Transfer(m_HalfHeight, "half_height");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Box, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Sphere, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Capsule, , Transfer, void)
