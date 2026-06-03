#pragma once

class NonCopyable
{
public:
    constexpr NonCopyable() = default;

private:
    NonCopyable(const NonCopyable&);
    NonCopyable& operator=(const NonCopyable&);
};