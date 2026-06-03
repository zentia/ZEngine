#pragma once

template<typename T>
struct AlignOfType
{
    enum
    {
        Align = alignof(T)
    };
};
