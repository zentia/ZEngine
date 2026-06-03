#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "TextureDefines.h"

#include <stdint.h>

class Texture : public Object
{
private:
    TextureFilter m_Filter;
    uint8_t m_Srbg;
};