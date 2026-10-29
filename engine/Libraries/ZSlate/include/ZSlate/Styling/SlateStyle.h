#pragma once

#include "ZSlate/Core/SlateBrush.h"
#include "ZSlate/Core/ZSlateTypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZSlate
{

// =============================================================================
// FSlateStyle — named style set (UE FSlateStyleSet analogue)
// =============================================================================
//
// Registers brushes, colors, fonts under string keys. Widgets resolve
// styles via static Get() registry, eliminating hardcoded colors.
//
// Usage:
//   auto style = FSlateStyle::Create("MyStyle");
//   style->SetColor("Button.Bg", UIColor(0.12f, 0.12f, 0.14f, 1.0f));
//   style->SetColor("Button.Hover", UIColor(0.16f, 0.17f, 0.21f, 1.0f));
//   ...
//   auto bg = FSlateStyle::GetColor("Button.Bg");
//
class FSlateStyle
{
public:
    explicit FSlateStyle(const std::string& name) : m_Name(name) {}

    static std::shared_ptr<FSlateStyle> Create(const std::string& name);
    static const FSlateStyle* Get(const std::string& name);
    static const FSlateStyle* GetCoreStyle();  // built-in default
    static UIColor GetColor(const std::string& key, const UIColor& fallback = Colors::White);

    const std::string& GetName() const { return m_Name; }

    // --- Setters (fluent) ----------------------------------------------------

    FSlateStyle& SetColor(const std::string& key, const UIColor& color)
    {
        m_Colors[key] = color;
        return *this;
    }

    FSlateStyle& SetBrush(const std::string& key, const FSlateBrush& brush)
    {
        m_Brushes[key] = brush;
        return *this;
    }

    FSlateStyle& SetFloat(const std::string& key, float value)
    {
        m_Floats[key] = value;
        return *this;
    }

    FSlateStyle& SetMargin(const std::string& key, const FMargin& margin)
    {
        m_Margins[key] = margin;
        return *this;
    }

    // --- Getters -------------------------------------------------------------

    UIColor GetColor(const std::string& key, const UIColor& fallback = Colors::White) const
    {
        auto it = m_Colors.find(key);
        return (it != m_Colors.end()) ? it->second : fallback;
    }

    const FSlateBrush* GetBrush(const std::string& key) const
    {
        auto it = m_Brushes.find(key);
        return (it != m_Brushes.end()) ? &it->second : nullptr;
    }

    float GetFloat(const std::string& key, float fallback = 0.0f) const
    {
        auto it = m_Floats.find(key);
        return (it != m_Floats.end()) ? it->second : fallback;
    }

    FMargin GetMargin(const std::string& key, const FMargin& fallback = FMargin()) const
    {
        auto it = m_Margins.find(key);
        return (it != m_Margins.end()) ? it->second : fallback;
    }

private:
    std::string m_Name;
    std::unordered_map<std::string, UIColor> m_Colors;
    std::unordered_map<std::string, FSlateBrush> m_Brushes;
    std::unordered_map<std::string, float> m_Floats;
    std::unordered_map<std::string, FMargin> m_Margins;
};

// =============================================================================
// FCoreStyle — built-in default style (UE FCoreStyle analogue)
// =============================================================================
//
// Provides default colors, fonts, and metrics for every widget type.
// Created automatically on first access.
//
class FCoreStyle
{
public:
    static void Initialize();
    static const FSlateStyle& Get();

    // Semantic color keys used by all widgets
    struct Colors
    {
        static constexpr const char* kPanelBg        = "Panel.Background";
        static constexpr const char* kInputBg        = "Input.Background";
        static constexpr const char* kInputBorder     = "Input.Border";
        static constexpr const char* kInputText       = "Input.Text";
        static constexpr const char* kInputHint       = "Input.Hint";
        static constexpr const char* kButtonBg        = "Button.Background";
        static constexpr const char* kButtonHover     = "Button.Hover";
        static constexpr const char* kButtonPressed   = "Button.Pressed";
        static constexpr const char* kButtonText      = "Button.Text";
        static constexpr const char* kCheckBoxBg      = "CheckBox.Background";
        static constexpr const char* kCheckBoxChecked = "CheckBox.Checked";
        static constexpr const char* kSliderTrack     = "Slider.Track";
        static constexpr const char* kSliderThumb     = "Slider.Thumb";
        static constexpr const char* kProgressBg      = "Progress.Background";
        static constexpr const char* kProgressFill    = "Progress.Fill";
        static constexpr const char* kScrollBarTrack  = "ScrollBar.Track";
        static constexpr const char* kScrollBarThumb  = "ScrollBar.Thumb";
        static constexpr const char* kSeparator       = "Separator";
        static constexpr const char* kSelection       = "Selection";
        static constexpr const char* kFocusBorder     = "Focus.Border";
        static constexpr const char* kHeaderBg        = "Header.Background";
        static constexpr const char* kHeaderText      = "Header.Text";
        static constexpr const char* kNotificationBg  = "Notification.Background";
        static constexpr const char* kTooltipBg       = "Tooltip.Background";
        static constexpr const char* kTooltipText     = "Tooltip.Text";
        static constexpr const char* kMenuBg          = "Menu.Background";
        static constexpr const char* kMenuItemHover   = "MenuItem.Hover";
        static constexpr const char* kDropTarget      = "DropTarget.Highlight";
    };

    struct Metrics
    {
        static constexpr const char* kDefaultFontSize = "Font.Size.Default";
        static constexpr const char* kSmallFontSize   = "Font.Size.Small";
        static constexpr const char* kButtonHeight     = "Widget.Button.Height";
        static constexpr const char* kInputHeight      = "Widget.Input.Height";
        static constexpr const char* kScrollBarWidth   = "Widget.ScrollBar.Width";
    };
};

// =============================================================================
// Inline implementations
// =============================================================================

inline std::shared_ptr<FSlateStyle> FSlateStyle::Create(const std::string& name)
{
    auto style = std::make_shared<FSlateStyle>(name);
    // Register in global registry
    static std::vector<std::shared_ptr<FSlateStyle>>& registry =
        *new std::vector<std::shared_ptr<FSlateStyle>>();
    registry.push_back(style);
    return style;
}

inline const FSlateStyle* FSlateStyle::Get(const std::string& name)
{
    static std::vector<std::shared_ptr<FSlateStyle>>& registry =
        *new std::vector<std::shared_ptr<FSlateStyle>>();
    for (const auto& s : registry)
        if (s->GetName() == name) return s.get();
    return nullptr;
}

inline const FSlateStyle* FSlateStyle::GetCoreStyle()
{
    return Get("CoreStyle");
}

inline UIColor FSlateStyle::GetColor(const std::string& key, const UIColor& fallback)
{
    if (auto* style = GetCoreStyle())
    {
        UIColor c = style->GetColor(key, fallback);
        if (c.x >= 0) return c;
    }
    return fallback;
}

inline void FCoreStyle::Initialize()
{
    auto style = FSlateStyle::Create("CoreStyle");

    // Panel
    style->SetColor(Colors::kPanelBg,           UIColor(0.08f, 0.08f, 0.10f, 1.0f));
    // Input
    style->SetColor(Colors::kInputBg,           UIColor(0.12f, 0.12f, 0.14f, 1.0f));
    style->SetColor(Colors::kInputBorder,       UIColor(0.30f, 0.32f, 0.38f, 1.0f));
    style->SetColor(Colors::kInputText,         UIColor(0.88f, 0.89f, 0.92f, 1.0f));
    style->SetColor(Colors::kInputHint,         UIColor(0.45f, 0.45f, 0.50f, 1.0f));
    // Button
    style->SetColor(Colors::kButtonBg,          UIColor(0.12f, 0.12f, 0.14f, 1.0f));
    style->SetColor(Colors::kButtonHover,       UIColor(0.16f, 0.17f, 0.21f, 1.0f));
    style->SetColor(Colors::kButtonPressed,     UIColor(0.20f, 0.22f, 0.28f, 1.0f));
    style->SetColor(Colors::kButtonText,        UIColor(0.88f, 0.89f, 0.92f, 1.0f));
    // CheckBox
    style->SetColor(Colors::kCheckBoxBg,        UIColor(0.20f, 0.20f, 0.22f, 1.0f));
    style->SetColor(Colors::kCheckBoxChecked,   UIColor(0.30f, 0.55f, 0.95f, 1.0f));
    // Slider
    style->SetColor(Colors::kSliderTrack,       UIColor(0.12f, 0.12f, 0.14f, 1.0f));
    style->SetColor(Colors::kSliderThumb,       UIColor(0.30f, 0.55f, 0.95f, 1.0f));
    // Progress
    style->SetColor(Colors::kProgressBg,        UIColor(0.12f, 0.12f, 0.14f, 1.0f));
    style->SetColor(Colors::kProgressFill,      UIColor(0.30f, 0.55f, 0.95f, 1.0f));
    // ScrollBar
    style->SetColor(Colors::kScrollBarTrack,    UIColor(0.12f, 0.12f, 0.14f, 1.0f));
    style->SetColor(Colors::kScrollBarThumb,    UIColor(0.28f, 0.30f, 0.36f, 1.0f));
    // Separator
    style->SetColor(Colors::kSeparator,         UIColor(0.08f, 0.08f, 0.10f, 1.0f));
    // Selection
    style->SetColor(Colors::kSelection,         UIColor(0.18f, 0.35f, 0.62f, 1.0f));
    style->SetColor(Colors::kFocusBorder,       UIColor(0.45f, 0.55f, 0.80f, 1.0f));
    // Header
    style->SetColor(Colors::kHeaderBg,          UIColor(0.10f, 0.10f, 0.12f, 1.0f));
    style->SetColor(Colors::kHeaderText,        UIColor(0.80f, 0.82f, 0.88f, 1.0f));
    // Notification
    style->SetColor(Colors::kNotificationBg,    UIColor(0.12f, 0.16f, 0.24f, 0.95f));
    // Tooltip
    style->SetColor(Colors::kTooltipBg,         UIColor(0.05f, 0.05f, 0.08f, 0.95f));
    style->SetColor(Colors::kTooltipText,       UIColor(0.85f, 0.88f, 0.94f, 1.0f));
    // Menu
    style->SetColor(Colors::kMenuBg,            UIColor(0.14f, 0.15f, 0.18f, 0.97f));
    style->SetColor(Colors::kMenuItemHover,     UIColor(0.20f, 0.35f, 0.60f, 1.0f));
    style->SetColor(Colors::kDropTarget,        UIColor(0.12f, 0.30f, 0.50f, 0.85f));

    // Metrics
    style->SetFloat(FCoreStyle::Metrics::kDefaultFontSize, 14.0f);
    style->SetFloat(FCoreStyle::Metrics::kSmallFontSize,   11.0f);
    style->SetFloat(FCoreStyle::Metrics::kButtonHeight,    28.0f);
    style->SetFloat(FCoreStyle::Metrics::kInputHeight,     26.0f);
    style->SetFloat(FCoreStyle::Metrics::kScrollBarWidth,  12.0f);
}

inline const FSlateStyle& FCoreStyle::Get()
{
    if (FSlateStyle::GetCoreStyle() == nullptr)
        Initialize();
    return *FSlateStyle::GetCoreStyle();
}

}  // namespace ZSlate
