#pragma once

struct NullType
{
};

template<bool B, typename T, typename F>
struct Conditional
{
    using Type = T;
};

template<typename T, typename U, int equal_value, int unequal_value>
struct SelectOnTypeEquality
{
    enum
    {
        result = unequal_value
    };
};

template<typename T, int equal_value, int unequal_value>
struct SelectOnTypeEquality<T, T, equal_value, unequal_value>
{
    enum
    {
        result = equal_value
    };
};