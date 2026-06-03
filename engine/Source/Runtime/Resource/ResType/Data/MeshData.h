#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <string>
#include <vector>
class Vertex
{
public:
    DECLARE_SERIALIZE(Vertex)

    float px;
    float py;
    float pz;
    float nx;
    float ny;
    float nz;
    float tx;
    float ty;
    float tz;
    float u;
    float v;
};

template<typename TransferFunction>
void Vertex::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(px, "px");
    transfer.Transfer(py, "py");
    transfer.Transfer(pz, "pz");
    transfer.Transfer(nx, "nx");
    transfer.Transfer(ny, "ny");
    transfer.Transfer(nz, "nz");
    transfer.Transfer(tx, "tx");
    transfer.Transfer(ty, "ty");
    transfer.Transfer(tz, "tz");
    transfer.Transfer(u, "u");
    transfer.Transfer(v, "v");
}

class SkeletonBinding
{
public:
    DECLARE_SERIALIZE(SkeletonBinding)

    int index0;
    int index1;
    int index2;
    int index3;
    float weight0;
    float weight1;
    float weight2;
    float weight3;
};

template<typename TransferFunction>
void SkeletonBinding::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(index0, "index0");
    transfer.Transfer(index1, "index1");
    transfer.Transfer(index2, "index2");
    transfer.Transfer(index3, "index3");
    transfer.Transfer(weight0, "weight0");
    transfer.Transfer(weight1, "weight1");
    transfer.Transfer(weight2, "weight2");
    transfer.Transfer(weight3, "weight3");
}

class MeshData : public Object
{
    REGISTER_CLASS(MeshData);
    DECLARE_OBJECT_SERIALIZE()

public:
    std::vector<Vertex> vertex_buffer;
    std::vector<int> index_buffer;
    std::vector<SkeletonBinding> bind;
};
