#pragma once

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/ZSlateTypes.h"

namespace ZSlate
{
// ============================================================================
// Brush Types - Reference UE FSlateBrush
// ============================================================================

// Drawing type enum - how to draw the brush
enum class ESlateBrushDrawType : uint8_t
{
    NoDrawType,     // Don't draw
    Box,            // 3x3 box, edges and center stretch according to Margin
    Border,         // 3x3 border, edges tile, center is empty
    Image,          // Draw image, ignore Margin
    RoundedBox      // Rounded rectangle with fill and optional border
};

// Tiling type enum
enum class ESlateBrushTileType : uint8_t
{
    NoTile,         // Stretch
    Horizontal,     // Horizontal tile
    Vertical,       // Vertical tile
    Both            // Both directions tile
};

// Mirror type enum
enum class ESlateBrushMirrorType : uint8_t
{
    NoMirror,       // No mirror
    Horizontal,     // Horizontal mirror
    Vertical,       // Vertical mirror
    Both            // Both directions mirror
};

// Rounded box outline settings
struct FSlateBrushOutlineSettings
{
    Vector4 CornerRadii{0.0f, 0.0f, 0.0f, 0.0f};  // XY=TopLeft, ZW=BottomRight
    UIColor Color{0.0f, 0.0f, 0.0f, 0.0f};         // Border color
    float Width{0.0f};                                 // Border width
    bool bUseBrushTransparency{false};                 // Use brush's transparency

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

// ============================================================================
// FSlateBrush - Brush for drawing Slate elements
// ============================================================================

struct FSlateBrush
{
public:
    // Drawing type
    ESlateBrushDrawType DrawAs{ESlateBrushDrawType::Image};
    
    // Tiling method
    ESlateBrushTileType Tiling{ESlateBrushTileType::NoTile};
    
    // Mirroring method
    ESlateBrushMirrorType Mirroring{ESlateBrushMirrorType::NoMirror};
    
    // Texture resource (backend-opaque handle, null=use solid color)
    void* Texture{nullptr};
    
    // Tint color
    UIColor Tint{1.0f, 1.0f, 1.0f, 1.0f};
    
    // Expected size (Slate units)
    Vector2 ImageSize{16.0f, 16.0f};
    
    // Margin (for Box and Border modes, UV space 0-1)
    FMargin Margin{0.0f, 0.0f, 0.0f, 0.0f};
    
    // UV region
    Vector2 Uv0{0.0f, 0.0f};
    Vector2 Uv1{1.0f, 1.0f};
    
    // Rounded box outline settings (for RoundedBox type)
    FSlateBrushOutlineSettings OutlineSettings;
    
    // Whether this brush is set (for optional brush detection)
    bool bIsSet{true};

public:
    // Constructors
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
    
    // Static factories
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
        Brush.Texture = nullptr;  // RoundedBox doesn't need texture
        return Brush;
    }

    // Getters
    ESlateBrushDrawType GetDrawType() const { return DrawAs; }
    const FMargin& GetMargin() const { return Margin; }
    ESlateBrushTileType GetTiling() const { return Tiling; }
    ESlateBrushMirrorType GetMirroring() const { return Mirroring; }
    bool IsSet() const { return bIsSet; }
    
    // Setters
    void SetDrawType(ESlateBrushDrawType InDrawAs) { DrawAs = InDrawAs; }
    void SetMargin(const FMargin& InMargin) { Margin = InMargin; }
    void SetTiling(ESlateBrushTileType InTiling) { Tiling = InTiling; }
    void SetMirroring(ESlateBrushMirrorType InMirroring) { Mirroring = InMirroring; }
    void SetImageSize(const Vector2& InSize) { ImageSize = InSize; }
    Vector2 GetImageSize() const { return ImageSize; }

    // Operators
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
