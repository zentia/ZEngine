#pragma once

#include <cstddef>
#include <mimalloc.h>
#include <utility>

// engine/source/runtime/core/memory/auto_memory_typed.h
// AutoMemoryTyped: RAII wrapper for mimalloc-allocated typed memory
// Automatically allocates memory on construction and deallocates on destruction

// Tag types for constructor disambiguation
struct AlignTag
{
};
struct CountTag
{
};

template<typename T>
class AutoMemoryTyped
{
public:
    // Default constructor - allocates memory for one T
    explicit AutoMemoryTyped()
        : m_Ptr(static_cast<T*>(mi_malloc(sizeof(T)))), m_IsObjectConstructed(false) {}

    // Constructor with alignment (single object)
    explicit AutoMemoryTyped(size_t alignment, AlignTag)
        : m_Ptr(static_cast<T*>(mi_malloc_aligned(sizeof(T), alignment))), m_IsObjectConstructed(false)
    {
    }

    // Constructor with count (for arrays)
    explicit AutoMemoryTyped(size_t count, CountTag)
        : m_Ptr(static_cast<T*>(mi_malloc(sizeof(T) * count))), m_Count(count), m_IsObjectConstructed(false)
    {
    }

    // Constructor with count and alignment (for aligned arrays)
    explicit AutoMemoryTyped(size_t count, size_t alignment)
        : m_Ptr(static_cast<T*>(mi_malloc_aligned(sizeof(T) * count, alignment))), m_Count(count),
          m_IsObjectConstructed(false)
    {
    }

    // Delete copy constructor and copy assignment
    AutoMemoryTyped(const AutoMemoryTyped&) = delete;
    AutoMemoryTyped& operator=(const AutoMemoryTyped&) = delete;

    // Move constructor
    AutoMemoryTyped(AutoMemoryTyped&& other) noexcept
        : m_Ptr(other.m_Ptr), m_Count(other.m_Count)
    {
        other.m_Ptr = nullptr;
        other.m_Count = 0;
    }

    // Move assignment
    AutoMemoryTyped& operator=(AutoMemoryTyped&& other) noexcept
    {
        if (this != &other)
        {
            FreeMemory();
            m_Ptr = other.m_Ptr;
            m_Count = other.m_Count;
            other.m_Ptr = nullptr;
            other.m_Count = 0;
        }
        return *this;
    }

    // Destructor - automatically frees memory
    ~AutoMemoryTyped() { FreeMemory(); }

    // Get raw pointer
    T* get() const noexcept { return m_Ptr; }

    // Get pointer to array element (bounds checked if count > 0)
    T* getAt(size_t index) const
    {
        if (m_Ptr == nullptr)
            return nullptr;
        if (m_Count > 0 && index >= m_Count)
            return nullptr;
        return m_Ptr + index;
    }

    // Get count (for arrays)
    size_t getCount() const noexcept { return m_Count; }

    // Check if valid
    bool IsValid() const noexcept { return m_Ptr != nullptr; }

    // Mark object as constructed (call this after using placement new)
    void markObjectConstructed() noexcept { m_IsObjectConstructed = true; }

    // Release ownership (does not free memory, returns raw pointer)
    // Note: if object was constructed, you should call its destructor before release
    T* release() noexcept
    {
        T* ptr = m_Ptr;
        m_Ptr = nullptr;
        m_Count = 0;
        m_IsObjectConstructed = false;
        return ptr;
    }

    // Reset - free current memory and allocate new
    void reset()
    {
        FreeMemory();
        m_Ptr = static_cast<T*>(mi_malloc(sizeof(T)));
        m_Count = 0;
        m_IsObjectConstructed = false;
    }

    // Reset with count
    void reset(size_t count)
    {
        FreeMemory();
        m_Ptr = static_cast<T*>(mi_malloc(sizeof(T) * count));
        m_Count = count;
        m_IsObjectConstructed = false;
    }

    // Reset with alignment
    void reset(size_t count, size_t alignment)
    {
        FreeMemory();
        m_Ptr = static_cast<T*>(mi_malloc_aligned(sizeof(T) * count, alignment));
        m_Count = count;
        m_IsObjectConstructed = false;
    }

    // Operator overloads for pointer-like access
    T* operator->() const noexcept { return m_Ptr; }

    T& operator*() const { return *m_Ptr; }

    // Array subscript operator (for array mode)
    T& operator[](size_t index) const { return m_Ptr[index]; }

    // Implicit conversion to bool (for null checks)
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }

private:
    T* m_Ptr = nullptr;
    size_t m_Count = 0;                  // 0 means single object, >0 means array
    bool m_IsObjectConstructed = false;  // Track if object was constructed using placement new

    void FreeMemory()
    {
        if (m_Ptr != nullptr)
        {
            // For single object, call destructor if it was constructed
            if (m_Count == 0 && m_IsObjectConstructed)
            {
                m_Ptr->~T();
            }
            // For arrays, user should manually destruct objects before this point
            // We only free the memory here
            mi_free(m_Ptr);
            m_Ptr = nullptr;
            m_Count = 0;
            m_IsObjectConstructed = false;
        }
    }
};