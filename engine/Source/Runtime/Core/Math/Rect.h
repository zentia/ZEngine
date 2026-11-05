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

    // =========================================================================
    // Unity 的 `Rect` / `RectInt` 成员
    // =========================================================================
    //
    // `RectT<float>` 就是脚本侧的 `Rect`，`RectT<int>` 就是 `RectInt`（两个
    // using 在文件末尾）。Unity 的那批成员挂在模板上，因为两种 T 语义完全一样。
    //
    // ⚠️ **负的 width / height 是合法的**，而且 `min` / `max` 会**归一化**：
    // Unity 内部存的是两条边（m_XMin / m_XMax），`x` 是较小那条、`width` 是
    // `xMax - xMin`。这里存的是 x + width，所以那两个 getter 要自己做
    // Min/Max —— 直接返回 `x` 和 `x + width` 的话，一个 width 为负的矩形
    // （从右下往左上拖出来的那种）求值就全错了。
    //
    // ⚠️ `Contains` 是**半开**区间 `[min, max)` —— 右边界和上边界**不算**在内。
    // 这与 `Bounds.Contains`（闭区间，边界算包含）相反，是 Unity 里最出名的一
    // 处不一致。`Overlaps` 同样用严格不等号：贴边不算相交。

    /// Unity 的 `rect.xMin`：两条边里较小的那条。
    T GetXMin() const { return (x + width < x) ? (x + width) : x; }
    /// Unity 的 `rect.xMax`：较大的那条。
    T GetXMax() const { return (x + width < x) ? x : (x + width); }
    T GetYMin() const { return (y + height < y) ? (y + height) : y; }
    T GetYMax() const { return (y + height < y) ? y : (y + height); }

    /// Unity 的 `rect.width`（就是字段本身）。留着是为了让 `GetSize` 那组读起来
    /// 对称。
    T GetWidth() const { return width; }
    T GetHeight() const { return height; }

    /// Unity 的 `rect.Set(x, y, width, height)`。
    void Set(T new_x, T new_y, T new_width, T new_height)
    {
        x = new_x;
        y = new_y;
        width = new_width;
        height = new_height;
    }

    /// Unity 的 `rect.SetMinMax(min, max)`：由两条**对角**定矩形（顺序不限）。
    void SetMinMax(T min_x, T min_y, T max_x, T max_y)
    {
        x = min_x;
        y = min_y;
        width = max_x - min_x;
        height = max_y - min_y;
    }

    /// Unity 的 `rect.Contains(point)`。⚠️ 半开区间：`xMax` / `yMax` **不算**在内。
    bool Contains(T point_x, T point_y) const
    {
        return point_x >= GetXMin() && point_x < GetXMax() &&
               point_y >= GetYMin() && point_y < GetYMax();
    }

    /// Unity 的 `rect.Overlaps(other)`。⚠️ 严格不等号：贴边不算相交（两个矩形
    /// 只共享一条边时返回 false）。
    bool Overlaps(const RectType& other) const
    {
        return other.GetXMax() > GetXMin() && other.GetXMin() < GetXMax() &&
               other.GetYMax() > GetYMin() && other.GetYMin() < GetYMax();
    }

    /// Unity 的 `RectInt.ClampToBounds(bounds)`：把这个矩形挪进 `bounds`，尺寸
    /// 尽量保持不变（比 bounds 大时就顶到 bounds 边上）。
    ///
    /// `Rect`（float）在 Unity 里**没有**这个方法 —— 门面只在 `RectInt` 上导出。
    void ClampToBounds(const RectType& bounds)
    {
        const T bounds_width = bounds.GetXMax() - bounds.GetXMin();
        const T bounds_height = bounds.GetYMax() - bounds.GetYMin();

        T clamped_width = GetXMax() - GetXMin();
        if (clamped_width > bounds_width)
        {
            clamped_width = bounds_width;
        }
        T clamped_height = GetYMax() - GetYMin();
        if (clamped_height > bounds_height)
        {
            clamped_height = bounds_height;
        }

        T clamped_x = x;
        if (clamped_x < bounds.GetXMin())
        {
            clamped_x = bounds.GetXMin();
        }
        if (clamped_x > bounds.GetXMax() - clamped_width)
        {
            clamped_x = bounds.GetXMax() - clamped_width;
        }
        T clamped_y = y;
        if (clamped_y < bounds.GetYMin())
        {
            clamped_y = bounds.GetYMin();
        }
        if (clamped_y > bounds.GetYMax() - clamped_height)
        {
            clamped_y = bounds.GetYMax() - clamped_height;
        }

        x = clamped_x;
        y = clamped_y;
        width = clamped_width;
        height = clamped_height;
    }

    /// Unity 的 `Rect == Rect`：四个字段全等（**不**归一化 —— `(0,0,10,10)` 与
    /// `(10,10,-10,-10)` 在 `Contains` / `min` / `max` 上完全一致，但 `==` 是
    /// false）。这与 `Vector2 == Vector2` 一样是逐字段的值相等。
    bool operator==(const RectType& other) const
    {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
    bool operator!=(const RectType& other) const { return !(*this == other); }

    T x;
    T y;
    T width;
    T height;
};

/// Unity 的 `UnityEngine.Rect`（float）。
///
/// 名字是 `Rectf` 而不是 `Rect`：Apple 平台（iOS/macOS）的 `MacTypes.h` 会在
/// **全局命名空间**声明 `struct Rect`，而 Foundation / Metal / CoreGraphics
/// 任何一条 include 链都会把它带进来。C++ 里类型别名与类名同名即
/// "definition of type 'Rect' conflicts with type alias of the same name"，
/// 且与 include 先后无关 —— 所以引擎的值类型不能叫 `::Rect`。
/// Unity 自己的原生代码也是同样的取舍（`Rectf` / `RectInt`）。
/// 脚本侧对外的 `Rect` 名字由 `namespace Runtime` 的门面类提供（ScriptRect.h），
/// 它是有命名空间限定的，不会与 SDK 冲突。
using Rectf = RectT<float>;
/// Unity 的 `UnityEngine.RectInt`（int）。
using RectInt = RectT<int>;