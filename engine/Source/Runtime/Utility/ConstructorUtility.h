#pragma once
#include <type_traits>

enum NonNullPlacementT
{
    NonNullPlacement
};
inline void* operator new(size_t, void* ptr, NonNullPlacementT)
{
    return ptr;
}

inline void operator delete(void*, void*, NonNullPlacementT) {}

template<typename T>
struct AllocatorTraitsImpl
{
    inline static T* Construct(void* data) { return ::new (data, NonNullPlacement) T(); }

    inline static T* Construct(void* data, const T& other) { return ::new (data, NonNullPlacement) T(other); }

    template<typename... Args>
    static T* ConstructNew(Args&&... args)
    {
        return new (alignof(T), __FILE__, __LINE__) T(std::forward<Args>(args)...);
    }

    template<typename... Args>
    inline static T* Construct(void* data, Args&&... args)
    {
        return ::new (data, NonNullPlacement) T(std::forward<Args>(args)...);
    }
};

template<typename T>
struct AutoLabelConstructor
{
    using ValueType = typename std::remove_const<T>::type;
    using AllocatorTrait = AllocatorTraitsImpl<ValueType>;

    template<typename... Args>
    inline static ValueType* ConstructArgs(void* mem, Args&&... args)
    {
        return AllocatorTrait::template Construct<Args...>(mem, std::forward<Args>(args)...);
    }
};
