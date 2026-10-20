#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "BasicShape.generated.h"

ZCLASS(Abstract)
class Geometry : public Object
{
    ZGENERATED_BODY(Geometry)

public:
    virtual ~Geometry() {}
};

ZCLASS()
class Box : public Geometry
{
    ZGENERATED_BODY(Box)

public:
    ZPROPERTY()
    Vector3 m_HalfExtents {0.5f, 0.5f, 0.5f};
};

ZCLASS()
class Sphere : public Geometry
{
    ZGENERATED_BODY(Sphere)

public:
    ~Sphere() override {}
    ZPROPERTY()
    float m_Radius {0.5f};
};

ZCLASS()
class Capsule : public Geometry
{
    ZGENERATED_BODY(Capsule)

public:
    ~Capsule() override {}
    ZPROPERTY()
    float m_Radius {0.3f};
    ZPROPERTY()
    float m_HalfHeight {0.7f};
};

