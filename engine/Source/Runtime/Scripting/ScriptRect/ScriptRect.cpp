#include "Runtime/Scripting/ScriptRect/ScriptRect.h"

#include <cstdio>

namespace Runtime
{

    // =====================================================================
    // Rect（float）
    // =====================================================================

    ::Rectf Rect::Create(float x, float y, float width, float height)
    {
        return ::Rectf(x, y, width, height);
    }

    ::Rectf Rect::MinMaxRect(float xmin, float ymin, float xmax, float ymax)
    {
        ::Rectf r;
        r.SetMinMax(xmin, ymin, xmax, ymax);
        return r;
    }

    float Rect::GetXMin(const ::Rectf& r) { return r.GetXMin(); }
    float Rect::GetXMax(const ::Rectf& r) { return r.GetXMax(); }
    float Rect::GetYMin(const ::Rectf& r) { return r.GetYMin(); }
    float Rect::GetYMax(const ::Rectf& r) { return r.GetYMax(); }

    Vector2 Rect::GetMin(const ::Rectf& r) { return Vector2(r.GetXMin(), r.GetYMin()); }
    Vector2 Rect::GetMax(const ::Rectf& r) { return Vector2(r.GetXMax(), r.GetYMax()); }

    Vector2 Rect::GetSize(const ::Rectf& r) { return Vector2(r.width, r.height); }
    Vector2 Rect::GetPosition(const ::Rectf& r) { return Vector2(r.x, r.y); }
    Vector2 Rect::GetCenter(const ::Rectf& r)
    {
        return Vector2(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
    }

    ::Rectf Rect::WithXMin(const ::Rectf& r, float v)
    {
        ::Rectf out = r;
        out.x = v;
        out.width = r.GetXMax() - v;
        return out;
    }

    ::Rectf Rect::WithXMax(const ::Rectf& r, float v)
    {
        ::Rectf out = r;
        out.x = r.GetXMin();
        out.width = v - r.GetXMin();
        return out;
    }

    ::Rectf Rect::WithYMin(const ::Rectf& r, float v)
    {
        ::Rectf out = r;
        out.y = v;
        out.height = r.GetYMax() - v;
        return out;
    }

    ::Rectf Rect::WithYMax(const ::Rectf& r, float v)
    {
        ::Rectf out = r;
        out.y = r.GetYMin();
        out.height = v - r.GetYMin();
        return out;
    }

    ::Rectf Rect::WithLayout(const ::Rectf& r, float x, float y, float width, float height)
    {
        ::Rectf out = r;
        out.Set(x, y, width, height);
        return out;
    }

    ::Rectf Rect::WithMin(const ::Rectf& r, const Vector2& min)
    {
        ::Rectf out = r;
        out.x = min.x;
        out.y = min.y;
        out.width = r.GetXMax() - min.x;
        out.height = r.GetYMax() - min.y;
        return out;
    }

    ::Rectf Rect::WithMax(const ::Rectf& r, const Vector2& max)
    {
        ::Rectf out = r;
        out.x = r.GetXMin();
        out.y = r.GetYMin();
        out.width = max.x - r.GetXMin();
        out.height = max.y - r.GetYMin();
        return out;
    }

    ::Rectf Rect::WithSize(const ::Rectf& r, const Vector2& size)
    {
        ::Rectf out = r;
        out.width = size.x;
        out.height = size.y;
        return out;
    }

    ::Rectf Rect::WithPosition(const ::Rectf& r, const Vector2& position)
    {
        ::Rectf out = r;
        out.x = position.x;
        out.y = position.y;
        return out;
    }

    ::Rectf Rect::WithCenter(const ::Rectf& r, const Vector2& center)
    {
        ::Rectf out = r;
        // center == x + width / 2 对负 width 同样成立（xMin = x + width、
        // xMax = x，两者中点还是 x + width/2），所以这里不用先归一化。
        out.x = center.x - r.width * 0.5f;
        out.y = center.y - r.height * 0.5f;
        return out;
    }

    bool Rect::ContainsPoint(const ::Rectf& r, const Vector2& point)
    {
        return r.Contains(point.x, point.y);
    }

    bool Rect::ContainsXY(const ::Rectf& r, float x, float y) { return r.Contains(x, y); }

    bool Rect::Overlaps(const ::Rectf& a, const ::Rectf& b) { return a.Overlaps(b); }

    ::Rectf Rect::OrderMinMax(const ::Rectf& r)
    {
        ::Rectf out;
        out.x = r.GetXMin();
        out.y = r.GetYMin();
        out.width = r.GetXMax() - r.GetXMin();
        out.height = r.GetYMax() - r.GetYMin();
        return out;
    }

    Vector2 Rect::PointToNormalized(const ::Rectf& r, const Vector2& p)
    {
        const float span_x = r.GetXMax() - r.GetXMin();
        const float span_y = r.GetYMax() - r.GetYMin();
        // 退化的边（宽或高为 0）在 Unity 里会得到 0 / 0f == NaN。这里同样不
        // 夹取（矩形外的点照样超出 0..1），但把 0 宽这条边的结果定为 0 而不是
        // NaN —— NaN 一旦进了脚本就会静默污染后面所有比较。
        const float u = (span_x != 0.f) ? ((p.x - r.GetXMin()) / span_x) : 0.f;
        const float v = (span_y != 0.f) ? ((p.y - r.GetYMin()) / span_y) : 0.f;
        return Vector2(u, v);
    }

    Vector2 Rect::NormalizedToPoint(const ::Rectf& r, const Vector2& uv)
    {
        return Vector2(r.GetXMin() + uv.x * (r.GetXMax() - r.GetXMin()),
                       r.GetYMin() + uv.y * (r.GetYMax() - r.GetYMin()));
    }

    eastl::string Rect::ToString(const ::Rectf& r)
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "(x:%.2f, y:%.2f, width:%.2f, height:%.2f)", r.x, r.y,
                      r.width, r.height);
        return eastl::string(buffer);
    }

    // =====================================================================
    // RectInt
    // =====================================================================

    ::RectInt RectInt::Create(int x, int y, int width, int height)
    {
        return ::RectInt(x, y, width, height);
    }

    ::RectInt RectInt::MinMaxRect(int xmin, int ymin, int xmax, int ymax)
    {
        ::RectInt r;
        r.SetMinMax(xmin, ymin, xmax, ymax);
        return r;
    }

    int RectInt::GetXMin(const ::RectInt& r) { return r.GetXMin(); }
    int RectInt::GetXMax(const ::RectInt& r) { return r.GetXMax(); }
    int RectInt::GetYMin(const ::RectInt& r) { return r.GetYMin(); }
    int RectInt::GetYMax(const ::RectInt& r) { return r.GetYMax(); }

    ::Vector2Int RectInt::GetMin(const ::RectInt& r)
    {
        return ::Vector2Int(r.GetXMin(), r.GetYMin());
    }

    ::Vector2Int RectInt::GetMax(const ::RectInt& r)
    {
        return ::Vector2Int(r.GetXMax(), r.GetYMax());
    }

    ::Vector2Int RectInt::GetSize(const ::RectInt& r) { return ::Vector2Int(r.width, r.height); }

    ::Vector2Int RectInt::GetPosition(const ::RectInt& r) { return ::Vector2Int(r.x, r.y); }

    Vector2 RectInt::GetCenter(const ::RectInt& r)
    {
        // 整数矩形的中心可以落在半格上 —— Unity 也是返回 Vector2（float）。
        return Vector2(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
    }

    ::RectInt RectInt::WithMinMax(const ::RectInt& r, const ::Vector2Int& min,
                                  const ::Vector2Int& max)
    {
        ::RectInt out = r;
        out.SetMinMax(min.x, min.y, max.x, max.y);
        return out;
    }

    ::RectInt RectInt::WithMin(const ::RectInt& r, const ::Vector2Int& min)
    {
        ::RectInt out = r;
        out.x = min.x;
        out.y = min.y;
        out.width = r.GetXMax() - min.x;
        out.height = r.GetYMax() - min.y;
        return out;
    }

    ::RectInt RectInt::WithMax(const ::RectInt& r, const ::Vector2Int& max)
    {
        ::RectInt out = r;
        out.x = r.GetXMin();
        out.y = r.GetYMin();
        out.width = max.x - r.GetXMin();
        out.height = max.y - r.GetYMin();
        return out;
    }

    ::RectInt RectInt::WithSize(const ::RectInt& r, const ::Vector2Int& size)
    {
        ::RectInt out = r;
        out.width = size.x;
        out.height = size.y;
        return out;
    }

    ::RectInt RectInt::WithPosition(const ::RectInt& r, const ::Vector2Int& position)
    {
        ::RectInt out = r;
        out.x = position.x;
        out.y = position.y;
        return out;
    }

    ::RectInt RectInt::ClampToBounds(const ::RectInt& r, const ::RectInt& bounds)
    {
        ::RectInt out = r;
        out.ClampToBounds(bounds);
        return out;
    }

    bool RectInt::ContainsPoint(const ::RectInt& r, const ::Vector2Int& point)
    {
        return r.Contains(point.x, point.y);
    }

    bool RectInt::ContainsXY(const ::RectInt& r, int x, int y) { return r.Contains(x, y); }

    bool RectInt::Overlaps(const ::RectInt& a, const ::RectInt& b) { return a.Overlaps(b); }

    eastl::string RectInt::ToString(const ::RectInt& r)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "(x:%d, y:%d, width:%d, height:%d)", r.x, r.y, r.width,
                      r.height);
        return eastl::string(buffer);
    }

}  // namespace Runtime
