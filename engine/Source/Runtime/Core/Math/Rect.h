#pragma once

template<typename T>
class RectT
{
public:
    using RectType = RectT<T>;
    RectT()
        : x(0), y(0), width(0), height(0) {}
    RectT(T inX, T inY, T inWidth, T inHeight)
    {
        x = inX;
        y = inY;
        width = inWidth;
        height = inHeight;
    }

    inline void Scale(T dx, T dy)
    {
        x *= dx;
        width *= dx;
        y *= dy;
        height *= dy;
    }

    inline void Move(T dx, T dy)
    {
        x += dx;
        y += dy;
    }

    void Clamp(const RectType& r)
    {
        T x2 = x + width;
        T y2 = y + height;
        T rx2 = r.x + r.width;
        T ry2 = r.y + r.height;

        if (x < r.x)
            x = r.x;
        if (x2 > rx2)
            x2 = rx2;
        if (y < r.y)
            y = r.y;
        if (y2 > ry2)
            y2 = ry2;

        width = x2 - x;
        if (width < 0)
            width = 0;

        height = y2 - y;
        if (height < 0)
            height = 0;
    }

    T x;
    T y;
    T width;
    T height;
};

using Rectf = RectT<float>;