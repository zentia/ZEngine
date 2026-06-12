#pragma once

#include <cstdint>

#include "SlateEnums.h"
#include "SlateGeometry.h"
#include "Runtime/Core/Math/Vector4.h"

namespace ZSlate
{

// 绘制类型枚举 - 参考 UE FSlateBrushDrawType
enum class ESlateBrushDrawType : uint8_t
{
    NoDrawType,     // 不绘制
    Box,            // 3x3 box，边和中间根据 Margin 拉伸
    Border,         // 3x3 border，边平铺，中间为空
    Image,          // 绘制图像，忽略 Margin
    RoundedBox      // 带圆角的实心矩形
};

// 平铺类型枚举 - 参考 UE FSlateBrushTileType
enum class ESlateBrushTileType : uint8_t
{
    NoTile,         // 拉伸
    Horizontal,     // 水平平铺
    Vertical,       // 垂直平铺
    Both            // 双向平铺
};

// 镜像类型枚举 - 参考 UE FSlateBrushMirrorType
enum class ESlateBrushMirrorType : uint8_t
{
    NoMirror,       // 不镜像
    Horizontal,     // 水平镜像
    Vertical,       // 垂直镜像
    Both            // 双向镜像
};

// 圆角边框设置 - 用于 RoundedBox 类型
struct FSlateBrushOutlineSettings
{
    Vector4 CornerRadii{0.0f, 0.0f, 0.0f, 0.0f};  // XY=TopLeft, ZW=BottomRight (简化: 统一半径)
    UIColor Color{0.0f, 0.0f, 0.0f, 0.0f};         // 边框颜色 (透明=无边框)
    float Width{0.0f};                                 // 边框宽度 (0=无边框)
    bool bUseBrushTransparency{false};                 // 是否使用 Brush 的透明度

    FSlateBrushOutlineSettings() = default;
    
    FSlateBrushOutlineSettings(float InUniformRadius)
        : CornerRadii(InUniformRadius, InUniformRadius, InUniformRadius, InUniformRadius)
        , Width(0.0f)
        , bUseBrushTransparency(false)
    {}
    
    FSlateBrushOutlineSettings(float InUniformRadius, const UIColor& InColor, float InWidth)
        : CornerRadii(InUniformRadius, InUniformRadius, InUniformRadius, InUniformRadius)
        , Color(InColor)
        , Width(InWidth)
        , bUseBrushTransparency(false)
    {}
    
    bool operator==(const FSlateBrushOutlineSettings& Other) const
    {
        return CornerRadii == Other.CornerRadii
            && Color == Other.Color
            && Width == Other.Width
            && bUseBrushTransparency == Other.bUseBrushTransparency;
    }
};

/**
 * Slate Brush - 包含如何绘制 Slate 元素的信息
 * 参考 UE FSlateBrush
 */
struct FSlateBrush
{
public:
    // 绘制类型
    ESlateBrushDrawType DrawAs{ESlateBrushDrawType::Image};
    
    // 平铺方式
    ESlateBrushTileType Tiling{ESlateBrushTileType::NoTile};
    
    // 镜像方式
    ESlateBrushMirrorType Mirroring{ESlateBrushMirrorType::NoMirror};
    
    // 纹理资源 (后端不透明句柄，null=无纹理，使用纯色)
    void* Texture{nullptr};
    
    // 色调
    UIColor Tint{1.0f, 1.0f, 1.0f, 1.0f};
    
    // 期望大小 (Slate 单位)
    Vector2 ImageSize{16.0f, 16.0f};
    
    // Margin (用于 Box 和 Border 模式，UV 空间 0-1)
    FMargin Margin{0.0f, 0.0f, 0.0f, 0.0f};
    
    // UV 区域
    Vector2 Uv0{0.0f, 0.0f};
    Vector2 Uv1{1.0f, 1.0f};
    
    // 圆角边框设置 (仅用于 RoundedBox)
    FSlateBrushOutlineSettings OutlineSettings;
    
    // 是否设置了 Brush (用于可选 Brush 检测)
    bool bIsSet{true};

public:
    // 构造函数
    FSlateBrush() = default;
    
    explicit FSlateBrush(ESlateBrushDrawType InDrawAs)
        : DrawAs(InDrawAs)
    {}
    
    FSlateBrush(ESlateBrushDrawType InDrawAs, void* InTexture, const UIColor& InTint = UIColor(1,1,1,1))
        : DrawAs(InDrawAs)
        , Texture(InTexture)
        , Tint(InTint)
        , bIsSet(true)
    {}
    
    // 常用静态工厂
    static FSlateBrush Image(void* InTexture, const UIColor& InTint = UIColor(1,1,1,1))
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::Image;
        Brush.Texture = InTexture;
        Brush.Tint = InTint;
        return Brush;
    }
    
    static FSlateBrush Box(void* InTexture, const FMargin& InMargin, const UIColor& InTint = UIColor(1,1,1,1))
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::Box;
        Brush.Texture = InTexture;
        Brush.Margin = InMargin;
        Brush.Tint = InTint;
        return Brush;
    }
    
    static FSlateBrush Border(void* InTexture, const FMargin& InMargin, const UIColor& InTint = UIColor(1,1,1,1))
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::Border;
        Brush.Texture = InTexture;
        Brush.Margin = InMargin;
        Brush.Tint = InTint;
        return Brush;
    }
    
    static FSlateBrush RoundedBox(float InRadius, const UIColor& InFillColor, 
                                  const UIColor& InBorderColor = UIColor(0,0,0,0), 
                                  float InBorderWidth = 0.0f)
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
        Brush.Tint = InFillColor;
        Brush.OutlineSettings = FSlateBrushOutlineSettings(InRadius, InBorderColor, InBorderWidth);
        Brush.Texture = nullptr;  // RoundedBox 不需要纹理
        return Brush;
    }

    // Getter
    ESlateBrushDrawType GetDrawType() const { return DrawAs; }
    const FMargin& GetMargin() const { return Margin; }
    ESlateBrushTileType GetTiling() const { return Tiling; }
    ESlateBrushMirrorType GetMirroring() const { return Mirroring; }
    bool IsSet() const { return bIsSet; }
    
    // Setter
    void SetDrawType(ESlateBrushDrawType InDrawAs) { DrawAs = InDrawAs; }
    void SetMargin(const FMargin& InMargin) { Margin = InMargin; }
    void SetTiling(ESlateBrushTileType InTiling) { Tiling = InTiling; }
    void SetMirroring(ESlateBrushMirrorType InMirroring) { Mirroring = InMirroring; }
    void SetImageSize(const Vector2& InSize) { ImageSize = InSize; }
    Vector2 GetImageSize() const { return ImageSize; }

    // 运算符
    bool operator==(const FSlateBrush& Other) const
    {
        return DrawAs == Other.DrawAs
            && Tiling == Other.Tiling
            && Mirroring == Other.Mirroring
            && Texture == Other.Texture
            && Tint == Other.Tint
            && ImageSize == Other.ImageSize
            && Margin == Other.Margin
            && Uv0 == Other.Uv0
            && Uv1 == Other.Uv1
            && OutlineSettings == Other.OutlineSettings
            && bIsSet == Other.bIsSet;
    }
    
    bool operator!=(const FSlateBrush& Other) const
    {
        return !(*this == Other);
    }
};

}  // namespace ZSlate
