#pragma once
#include "Runtime/Core/Math/Vector3.h"

class Color
{
public:
    DECLARE_SERIALIZE(Color);
    float r;
    float g;
    float b;
    float a;
    Vector3 toVector3() { return Vector3(r, g, b); }
};

template<typename TransferFunction>
void Color::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(r, "r");
    transfer.Transfer(g, "g");
    transfer.Transfer(b, "b");
    transfer.Transfer(a, "a");
}