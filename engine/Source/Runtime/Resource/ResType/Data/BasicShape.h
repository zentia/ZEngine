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
public:
    DECLARE_SERIALIZE(Box)

    Vector3 m_HalfExtents {0.5f, 0.5f, 0.5f};
};

class Sphere : public Geometry
{
    DECLARE_SERIALIZE(Sphere)

public:
    ~Sphere() override {}
    float m_Radius {0.5f};
};

class Capsule : public Geometry
{
public:
    DECLARE_SERIALIZE(Capsule)

    ~Capsule() override {}
    float m_Radius {0.3f};
    float m_HalfHeight {0.7f};
};