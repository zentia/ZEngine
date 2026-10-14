#include "ZSlate/Core/SlatePlatform.h"

namespace ZSlate
{

namespace
{
    ISlatePlatform* GPlatform = nullptr;
}

void SetPlatform(ISlatePlatform* platform)
{
    GPlatform = platform;
}

ISlatePlatform* GetPlatform()
{
    return GPlatform;
}

} // namespace ZSlate
