#pragma once

#define ENUM_FLAGS(T) DETAIL_ENUM_FLAGS(T, unsigned, )

#define DETAIL_ENUM_FLAGS(T, BACKING_TYPE, PREFIX)                                                 \
    PREFIX inline T operator|(const T left, const T right)                                         \
    {                                                                                              \
        return static_cast<T>(static_cast<BACKING_TYPE>(left) | static_cast<BACKING_TYPE>(right)); \
    }                                                                                              \
    PREFIX inline T operator&(const T left, const T right)                                         \
    {                                                                                              \
        return static_cast<T>(static_cast<BACKING_TYPE>(left) & static_cast<BACKING_TYPE>(right)); \
    }                                                                                              \
    PREFIX inline T operator^(const T left, const T right)                                         \
    {                                                                                              \
        return static_cast<T>(static_cast<BACKING_TYPE>(left) ^ static_cast<BACKING_TYPE>(right)); \
    }                                                                                              \
    PREFIX inline T operator~(const T flags)                                                       \
    {                                                                                              \
        return static_cast<T>(~static_cast<BACKING_TYPE>(flags));                                  \
    }                                                                                              \
    PREFIX inline T& operator|=(T& left, const T right)                                            \
    {                                                                                              \
        return left = left | right;                                                                \
    }                                                                                              \
    PREFIX inline T& operator&=(T& left, const T right)                                            \
    {                                                                                              \
        return left = left & right;                                                                \
    }