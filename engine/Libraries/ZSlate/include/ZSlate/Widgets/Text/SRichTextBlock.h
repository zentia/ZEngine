#pragma once

#include "ZSlate/Widgets/SLeafWidget.h"
#include "ZSlate/Core/ZSlateTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZSlate
{
// ============================================================================
// Text Decorators - Custom inline content in rich text
// ============================================================================

// Base class for text decorators (UE ITextDecorator analogue)
class ITextDecorator
{
public:
    virtual ~ITextDecorator() = default;

    // Tag name this decorator handles (e.g., "Hyperlink", "Image")
    virtual std::string GetTagName() const = 0;

    // Whether this decorator handles the given tag name
    virtual bool HandlesTag(const std::string& TagName) const { return TagName == GetTagName(); }

    // Create the widget for this decorator instance
    // TagAttributes: key-value pairs from the tag (e.g., {"url", "https://..."})
    virtual std::shared_ptr<SWidget> CreateDecoratorWidget(
        const std::unordered_map<std::string, std::string>& TagAttributes,
        float BaseFontSize,
        const UIColor& BaseColor) = 0;
};

// ============================================================================
// Hyperlink Decorator - Clickable links in rich text
// ============================================================================

// Callback type for hyperlink navigation
using FOnHyperlinkNavigate = std::function<void(const std::string& Url)>;

// Hyperlink decorator (UE FSlateHyperlinkRun analogue)
class SHyperlinkDecorator : public ITextDecorator
{
public:
    SHyperlinkDecorator() = default;

    std::string GetTagName() const override { return "Hyperlink"; }

    std::shared_ptr<SWidget> CreateDecoratorWidget(
        const std::unordered_map<std::string, std::string>& TagAttributes,
        float BaseFontSize,
        const UIColor& BaseColor) override;

    // Set callback for hyperlink navigation
    void SetOnNavigate(FOnHyperlinkNavigate InCallback) { OnNavigate = std::move(InCallback); }

private:
    FOnHyperlinkNavigate OnNavigate;
};

// ============================================================================
// Rich Text Tag Types
// ============================================================================

// Rich text tag types
enum class ERichTextTagType
{
    Plain,          // Plain text run
    Bold,           // <b> or <strong>
    Italic,         // <i> or <em>
    Underline,      // <u>
    Strikethrough,  // <s> or <strike>
    Color,          // <color=#RRGGBB>
    Size,           // <size=N>
    Hyperlink,      // <Hyperlink url="...">
    LineBreak,      // <br>
    Custom          // Custom decorator
};

// Rich text run - a segment of text with consistent styling
struct FRichTextRun
{
    ERichTextTagType Type {ERichTextTagType::Plain};
    std::string Text;              // Display text (for Plain/LineBreak)
    UIColor Color {Colors::White}; // Foreground color
    float FontSize {14.0f};        // Font size
    bool Bold {false};
    bool Italic {false};
    bool Underline {false};
    bool Strikethrough {false};

    // For Hyperlink type
    std::string Url;
    std::string HyperlinkText;

    // For Custom decorator
    std::string DecoratorTag;
    std::unordered_map<std::string, std::string> DecoratorAttributes;

    // Cached layout info
    mutable float CachedWidth {0.0f};
    mutable bool bWidthDirty {true};
};

// Rich text line - a collection of runs on one line
struct FRichTextLine
{
    std::vector<FRichTextRun> Runs;
    float LineHeight {0.0f};
    float Baseline {0.0f};

    // Layout state
    mutable float CachedWidth {0.0f};
    mutable bool bWidthDirty {true};
};

// ============================================================================
// Rich Text Parser
// ============================================================================

class FRichTextParser
{
public:
    FRichTextParser() = default;

    // Parse rich text string into lines and runs
    // Supported tags:
    //   <b>, <strong> - bold
    //   <i>, <em>    - italic
    //   <u>          - underline
    //   <s>, <strike> - strikethrough
    //   <color=#RRGGBB> or <color=#AARRGGBB> - color
    //   <size=N>     - font size
    //   <Hyperlink url="...">text</>
    //   <br>         - line break
    //   </> or </tag> - end tag
    std::vector<FRichTextLine> Parse(const std::string& Text, float BaseFontSize, const UIColor& BaseColor);

    // Escape special characters
    static std::string Escape(const std::string& Text);

    // Unescape
    static std::string Unescape(const std::string& Text);

private:
    // Parse a single run, handling tags
    FRichTextRun ParseRun(std::string& TextBuffer, float BaseFontSize, const UIColor& BaseColor,
                          const FRichTextRun& CurrentState);

    // Parse color from string
    UIColor ParseColor(const std::string& ColorStr) const;
};

// ============================================================================
// SRichTextBlock - Displays rich text with inline formatting
// ============================================================================

class SRichTextBlock : public SLeafWidget
{
public:
    // Rich text options
    struct FRichTextOptions
    {
        float FontSize {14.0f};
        UIColor Color {Colors::White};
        float LineHeightPercentage {1.2f};
        TextAnchor Justification {TextAnchor::MiddleLeft};
        bool AutoWrapText {false};
        float WrapTextAt {0.0f};
        FMargin Margin {0.0f};
        
        // Decorators for custom inline content
        std::vector<std::shared_ptr<ITextDecorator>> Decorators;
    };

    SRichTextBlock()
        : SRichTextBlock(FRichTextOptions{})
    {
    }

    explicit SRichTextBlock(const FRichTextOptions& InOptions)
        : Options(InOptions)
        , m_Parser()
    {
    }

    // Set the rich text content
    void SetText(const std::string& InText)
    {
        Text = InText;
        ParsedLines = m_Parser.Parse(Text, Options.FontSize, Options.Color);
        InvalidateLayout();
    }

    const std::string& GetText() const { return Text; }

    // Set text color
    void SetColor(const UIColor& InColor) { Options.Color = InColor; }

    // Set font size
    void SetFontSize(float InSize) { Options.FontSize = InSize; }

    // Set line height percentage
    void SetLineHeightPercentage(float InPercentage) { Options.LineHeightPercentage = InPercentage; }

    // Set justification
    void SetJustification(TextAnchor InJustification) { Options.Justification = InJustification; }

    // Set auto wrap
    void SetAutoWrapText(bool bInAutoWrap) { Options.AutoWrapText = bInAutoWrap; }

    // Set wrap width
    void SetWrapTextAt(float InWidth) { Options.WrapTextAt = InWidth; }

    // Add a custom decorator
    void AddDecorator(const std::shared_ptr<ITextDecorator>& Decorator)
    {
        Options.Decorators.push_back(Decorator);
    }

    // Set hyperlink navigation callback
    void SetOnHyperlinkNavigate(FOnHyperlinkNavigate InCallback)
    {
        HyperlinkDecorator = std::make_shared<SHyperlinkDecorator>();
        HyperlinkDecorator->SetOnNavigate(std::move(InCallback));
        AddDecorator(HyperlinkDecorator);
    }

    // Get desired size
    Vector2 ComputeDesiredSize() const override;

    // Paint the rich text
    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override;

    // Override to support text measurement
    ISlateTextMeasurer* m_TextMeasurer {nullptr};

private:
    // Compute layout for a single line
    void ComputeLineLayout(FRichTextLine& Line, float AvailableWidth) const;

    // Paint a single line
    void PaintLine(const FPaintContext& ctx, const UIRect& LineRect, const FRichTextLine& Line) const;

    // Paint a single run
    void PaintRun(const FPaintContext& ctx, const Vector2& Position, const FRichTextRun& Run) const;

    // Measure a run's width
    float MeasureRunWidth(const FRichTextRun& Run) const;

    // Invalidate cached layout
    void InvalidateLayout()
    {
        for (auto& Line : ParsedLines)
        {
            Line.bWidthDirty = true;
        }
    }

    std::string Text;
    FRichTextOptions Options;
    mutable std::vector<FRichTextLine> ParsedLines;
    FRichTextParser m_Parser;

    // Hyperlink decorator (created lazily)
    std::shared_ptr<SHyperlinkDecorator> HyperlinkDecorator;

    // Inline widgets (for custom decorators)
    mutable std::vector<std::shared_ptr<SWidget>> InlineWidgets;
};

// ============================================================================
// Convenience Functions
// ============================================================================

// Create a rich text block with default options
inline std::shared_ptr<SRichTextBlock> CreateRichTextBlock(const std::string& InText,
                                                           const SRichTextBlock::FRichTextOptions& Options = SRichTextBlock::FRichTextOptions{})
{
    auto Widget = std::make_shared<SRichTextBlock>(Options);
    Widget->SetText(InText);
    return Widget;
}

}  // namespace ZSlate
