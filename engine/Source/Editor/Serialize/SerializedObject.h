#pragma once

#include <vector>

class Object;

class SerializedObject
{
public:
    void Init(std::vector<Object*>& objs, Object* context);
};