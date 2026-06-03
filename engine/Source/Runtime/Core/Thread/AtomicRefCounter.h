#pragma once

#include <atomic>

class AtomicRefCounter : std::atomic_int
{
public:
    AtomicRefCounter()
        : std::atomic_int(1) {}

    void Retain() { fetch_add(1); }

    bool Release()
    {
        const int32_t beforeDecrement = fetch_sub(1);
        return beforeDecrement == 1;
    }

    int Count() const { return load(std::memory_order_relaxed); }
};