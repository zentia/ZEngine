#include "ZSlate/Widgets/Text/SRichTextBlock.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace ZSlate
{

// ============================================================================
// FRichTextParser Implementation
// ============================================================================

std::vector<FRichTextLine> FRichTextParser::Parse(const std::string& Text, float BaseFontSize, const UIColor& BaseColor)
{
    std::vector<FRichTextLine> Lines;
    if (Text.empty())
    {
        Lines.push_back(FRichTextLine{});
        return Lines;
    }

    // Initial state
    FRichTextRun CurrentState;
    CurrentState.Type = ERichTextTagType::Plain;
    CurrentState.Color = BaseColor;
    CurrentState.FontSize = BaseFontSize;

    std::string TextBuffer;
    FRichTextLine CurrentLine;

    size_t i = 0;
    while (i < Text.size())
    {
        char c = Text[i];

        // Check for opening tag '<'
        if (c == '<')
        {
            // Find closing '>'
            size_t ClosePos = Text.find('>', i);
            if (ClosePos != std::string::npos && ClosePos - i > 1)
            {
                std::string Tag = Text.substr(i + 1, ClosePos - i - 1);

                // Check for closing tag </>
                if (Tag == "/" || (Tag.size() > 1 && Tag[1] == '/'))
                {
                    // End tag - reset to base state for simplicity
                    CurrentState = FRichTextRun{};
                    CurrentState.Color = BaseColor;
                    CurrentState.FontSize = BaseFontSize;
                }
                else
                {
                    // Parse opening tag
                    // Handle self-closing tags like <br/>
                    bool bSelfClosing = Tag.back() == '/';
                    if (bSelfClosing)
                    {
                        Tag.pop_back();
                    }

                    // Parse tag name and attributes
                    std::string TagName;
                    std::unordered_map<std::string, std::string> Attributes;

                    size_t SpacePos = Tag.find(' ');
                    if (SpacePos != std::string::npos)
                    {
                        TagName = Tag.substr(0, SpacePos);
                        std::string AttrStr = Tag.substr(SpacePos + 1);

                        // Parse attributes
                        std::string AttrBuffer;
                        std::string CurrentAttrName;
                        std::string CurrentAttrValue;
                        bool bInValue = false;
                        char QuoteChar = '\0';

                        for (char ch : AttrStr)
                        {
                            if (!bInValue)
                            {
                                if (ch == ' ')
                                {
                                    // End of attribute
                                    if (!CurrentAttrName.empty() && !CurrentAttrValue.empty())
                                    {
                                        Attributes[CurrentAttrName] = CurrentAttrValue;
                                        CurrentAttrName.clear();
                                        CurrentAttrValue.clear();
                                    }
                                }
                                else if (ch == '=')
                                {
                                    CurrentAttrValue.clear();
                                    bInValue = true;
                                }
                                else
                                {
                                    if (!CurrentAttrName.empty() || std::isalnum(ch) || ch == '-')
                                    {
                                        CurrentAttrName += ch;
                                    }
                                }
                            }
                            else
                            {
                                if (QuoteChar == '\0')
                                {
                                    if (ch == '"' || ch == '\'')
                                    {
                                        QuoteChar = ch;
                                    }
                                    else
                                    {
                                        CurrentAttrValue += ch;
                                    }
                                }
                                else if (ch == QuoteChar)
                                {
                                    bInValue = false;
                                    QuoteChar = '\0';
                                }
                                else
                                {
                                    CurrentAttrValue += ch;
                                }
                            }
                        }

                        // Don't forget the last attribute
                        if (!CurrentAttrName.empty())
                        {
                            Attributes[CurrentAttrName] = CurrentAttrValue;
                        }
                    }
                    else
                    {
                        TagName = Tag;
                    }

                    // Convert to lowercase for comparison
                    std::string TagNameLower = TagName;
                    std::transform(TagNameLower.begin(), TagNameLower.end(), TagNameLower.begin(), ::tolower);

                    // Handle specific tags
                    if (TagNameLower == "b" || TagNameLower == "strong")
                    {
                        CurrentState.Bold = true;
                    }
                    else if (TagNameLower == "i" || TagNameLower == "em")
                    {
                        CurrentState.Italic = true;
                    }
                    else if (TagNameLower == "u")
                    {
                        CurrentState.Underline = true;
                    }
                    else if (TagNameLower == "s" || TagNameLower == "strike" || TagNameLower == "strikethrough")
                    {
                        CurrentState.Strikethrough = true;
                    }
                    else if (TagNameLower == "br")
                    {
                        // Line break - Flush current run and start new line
                        if (!TextBuffer.empty())
                        {
                            FRichTextRun Run = CurrentState;
                            Run.Text = TextBuffer;
                            Run.Type = ERichTextTagType::Plain;
                            TextBuffer.clear();
                            CurrentLine.Runs.push_back(Run);
                        }
                        Lines.push_back(CurrentLine);
                        CurrentLine = FRichTextLine{};
                    }
                    else if (TagNameLower == "color")
                    {
                        auto It = Attributes.find("color");
                        if (It != Attributes.end())
                        {
                            CurrentState.Color = ParseColor(It->second);
                        }
                    }
                    else if (TagNameLower == "size")
                    {
                        auto It = Attributes.find("size");
                        if (It != Attributes.end())
                        {
                            int Size = std::atoi(It->second.c_str());
                            if (Size > 0)
                            {
                                CurrentState.FontSize = static_cast<float>(Size);
                            }
                        }
                    }
                    else if (TagNameLower == "hyperlink")
                    {
                        auto UrlIt = Attributes.find("url");
                        if (UrlIt != Attributes.end())
                        {
                            CurrentState.Type = ERichTextTagType::Hyperlink;
                            CurrentState.Url = UrlIt->second;
                            CurrentState.Color = UIColor(0.0f, 0.5f, 1.0f, 1.0f); // Link blue
                        }
                    }
                }

                i = ClosePos + 1;
                continue;
            }
        }

        // Regular character
        TextBuffer += c;

        // Flush run when style changes
        if (!TextBuffer.empty() && (CurrentState.Bold || CurrentState.Italic || CurrentState.Underline || CurrentState.Strikethrough || CurrentState.Type != ERichTextTagType::Plain))
        {
            // For now, we'll handle style changes by creating separate runs
            // This is a simplification - full implementation would track style stack
        }

        i++;
    }

    // Flush remaining buffer
    if (!TextBuffer.empty())
    {
        FRichTextRun Run = CurrentState;
        Run.Text = TextBuffer;
        Run.Type = ERichTextTagType::Plain;
        CurrentLine.Runs.push_back(Run);
    }

    // Add last line if not empty
    if (!CurrentLine.Runs.empty())
    {
        Lines.push_back(CurrentLine);
    }

    // If no lines were created, create an empty one
    if (Lines.empty())
    {
        Lines.push_back(FRichTextLine{});
    }

    return Lines;
}

std::string FRichTextParser::Escape(const std::string& Text)
{
    std::string Result;
    Result.reserve(Text.size() + 10);

    for (char c : Text)
    {
        switch (c)
        {
        case '<': Result += "&lt;"; break;
        case '>': Result += "&gt;"; break;
        case '&': Result += "&amp;"; break;
        case '"': Result += "&quot;"; break;
        case '\'': Result += "&#39;"; break;
        default: Result += c; break;
        }
    }

    return Result;
}

std::string FRichTextParser::Unescape(const std::string& Text)
{
    std::string Result;
    Result.reserve(Text.size());

    size_t i = 0;
    while (i < Text.size())
    {
        if (Text[i] == '&')
        {
            if (Text.compare(i, 4, "&lt;") == 0)
            {
                Result += '<';
                i += 4;
            }
            else if (Text.compare(i, 4, "&gt;") == 0)
            {
                Result += '>';
                i += 4;
            }
            else if (Text.compare(i, 5, "&amp;") == 0)
            {
                Result += '&';
                i += 5;
            }
            else if (Text.compare(i, 6, "&quot;") == 0)
            {
                Result += '"';
                i += 6;
            }
            else if (Text.compare(i, 5, "&#39;") == 0)
            {
                Result += '\'';
                i += 5;
            }
            else
            {
                Result += Text[i];
                i++;
            }
        }
        else
        {
            Result += Text[i];
            i++;
        }
    }

    return Result;
}

UIColor FRichTextParser::ParseColor(const std::string& ColorStr) const
{
    UIColor Color(1.0f, 1.0f, 1.0f, 1.0f);

    // Remove # prefix if present
    std::string Str = ColorStr;
    if (!Str.empty() && Str[0] == '#')
    {
        Str = Str.substr(1);
    }

    // Convert to lowercase
    std::string LowerStr = Str;
    std::transform(LowerStr.begin(), LowerStr.end(), LowerStr.begin(), ::tolower);

    // Parse hex color
    if (LowerStr.size() == 6)
    {
        // RRGGBB
        Color.x = std::strtol(LowerStr.substr(0, 2).c_str(), nullptr, 16) / 255.0f;
        Color.y = std::strtol(LowerStr.substr(2, 2).c_str(), nullptr, 16) / 255.0f;
        Color.z = std::strtol(LowerStr.substr(4, 2).c_str(), nullptr, 16) / 255.0f;
        Color.w = 1.0f;
    }
    else if (LowerStr.size() == 8)
    {
        // AARRGGBB
        Color.w = std::strtol(LowerStr.substr(0, 2).c_str(), nullptr, 16) / 255.0f;
        Color.x = std::strtol(LowerStr.substr(2, 2).c_str(), nullptr, 16) / 255.0f;
        Color.y = std::strtol(LowerStr.substr(4, 2).c_str(), nullptr, 16) / 255.0f;
        Color.z = std::strtol(LowerStr.substr(6, 2).c_str(), nullptr, 16) / 255.0f;
    }

    return Color;
}

// ============================================================================
// SHyperlinkDecorator Implementation
// ============================================================================

std::shared_ptr<SWidget> SHyperlinkDecorator::CreateDecoratorWidget(
    const std::unordered_map<std::string, std::string>& TagAttributes,
    float BaseFontSize,
    const UIColor& BaseColor)
{
    // Create clickable text widget for hyperlink
    auto Text = std::make_shared<STextBlock>();
    std::string LinkText = TagAttributes.count("text") ? TagAttributes.at("text") : "";
    if (!LinkText.empty())
    {
        Text->SetText(LinkText);
        Text->FontSize = BaseFontSize;
        Text->Color = UIColor(0.0f, 0.5f, 1.0f, 1.0f); // Link blue
    }

    // Find URL attribute and store for click handling
    auto UrlIt = TagAttributes.find("url");
    if (UrlIt != TagAttributes.end())
    {
        // Store URL in a custom property for later use
        // For now, we'll use a simple approach - wrap in a clickable container
        auto Button = std::make_shared<SButton>();
        Button->SetContent(Text);
        
        // Store URL for click handling
        Button->OnClicked = [url = UrlIt->second]() {
            // URL navigation would be handled here
        };
        
        return Button;
    }

    return Text;
}

// ============================================================================
// SRichTextBlock Implementation
// ============================================================================

Vector2 SRichTextBlock::ComputeDesiredSize() const
{
    if (m_TextMeasurer == nullptr)
    {
        // Fallback estimate
        return Vector2(static_cast<float>(Text.size()) * Options.FontSize * 0.5f, Options.FontSize * 1.2f);
    }

    float MaxWidth = 0.0f;
    float TotalHeight = 0.0f;
    float LineHeight = Options.FontSize * Options.LineHeightPercentage;

    for (const auto& Line : ParsedLines)
    {
        if (Line.Runs.empty())
        {
            TotalHeight += LineHeight;
            continue;
        }

        float LineWidth = 0.0f;
        for (const auto& Run : Line.Runs)
        {
            if (Run.Type == ERichTextTagType::Hyperlink)
            {
                // Estimate hyperlink width
                LineWidth += static_cast<float>(Run.HyperlinkText.size()) * Run.FontSize * 0.5f;
            }
            else if (!Run.Text.empty())
            {
                LineWidth += m_TextMeasurer->Measure(Run.Text, Run.FontSize).x;
            }
        }

        MaxWidth = std::max(MaxWidth, LineWidth);
        TotalHeight += LineHeight;
    }

    return Vector2(MaxWidth + Options.Margin.Left + Options.Margin.Right,
                   TotalHeight + Options.Margin.Top + Options.Margin.Bottom);
}

void SRichTextBlock::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (ctx.Renderer == nullptr)
    {
        return;
    }

    // Apply margin offset
    Vector2 ContentOffset(Options.Margin.Left, Options.Margin.Top);
    float ContentWidth = geom.ToRect().Width() - Options.Margin.Left - Options.Margin.Right;
    float ContentHeight = geom.ToRect().Height() - Options.Margin.Top - Options.Margin.Bottom;

    float LineHeight = Options.FontSize * Options.LineHeightPercentage;
    float CurrentY = ContentOffset.y;

    for (const auto& Line : ParsedLines)
    {
        if (CurrentY > ContentHeight + LineHeight)
        {
            break; // Line is below visible area
        }

        if (Line.Runs.empty())
        {
            CurrentY += LineHeight;
            continue;
        }

        // Compute line width
        float LineWidth = 0.0f;
        for (const auto& Run : Line.Runs)
        {
            if (Run.Type == ERichTextTagType::Hyperlink)
            {
                LineWidth += static_cast<float>(Run.HyperlinkText.size()) * Run.FontSize * 0.5f;
            }
            else if (!Run.Text.empty())
            {
                if (m_TextMeasurer)
                {
                    LineWidth += m_TextMeasurer->Measure(Run.Text, Run.FontSize).x;
                }
                else
                {
                    LineWidth += static_cast<float>(Run.Text.size()) * Run.FontSize * 0.5f;
                }
            }
        }

        // Apply justification
        float LineX = ContentOffset.x;
        switch (Options.Justification)
        {
        case TextAnchor::MiddleRight:
            LineX += ContentWidth - LineWidth;
            break;
        case TextAnchor::MiddleCenter:
            LineX += (ContentWidth - LineWidth) * 0.5f;
            break;
        default:
            break;
        }

        // Paint runs
        float RunX = LineX;
        for (const auto& Run : Line.Runs)
        {
            Vector2 RunPos(RunX, CurrentY);

            switch (Run.Type)
            {
            case ERichTextTagType::Hyperlink:
                PaintRun(ctx, RunPos, Run);
                RunX += static_cast<float>(Run.HyperlinkText.size()) * Run.FontSize * 0.5f;
                break;
            default:
                if (!Run.Text.empty())
                {
                    PaintRun(ctx, RunPos, Run);
                    if (m_TextMeasurer)
                    {
                        RunX += m_TextMeasurer->Measure(Run.Text, Run.FontSize).x;
                    }
                    else
                    {
                        RunX += static_cast<float>(Run.Text.size()) * Run.FontSize * 0.5f;
                    }
                }
                break;
            }
        }

        CurrentY += LineHeight;
    }
}

void SRichTextBlock::ComputeLineLayout(FRichTextLine& /*Line*/, float /*AvailableWidth*/) const
{
    // Line layout is handled during parsing for now
    // Full word-wrap implementation would go here
}

void SRichTextBlock::PaintLine(const FPaintContext& ctx, const UIRect& /*LineRect*/, const FRichTextLine& Line) const
{
    // Paint runs
    float RunX = 0.0f;
    for (const auto& Run : Line.Runs)
    {
        PaintRun(ctx, Vector2(RunX, 0.0f), Run);
        if (!Run.Text.empty())
        {
            if (m_TextMeasurer)
            {
                RunX += m_TextMeasurer->Measure(Run.Text, Run.FontSize).x;
            }
            else
            {
                RunX += static_cast<float>(Run.Text.size()) * Run.FontSize * 0.5f;
            }
        }
    }
}

void SRichTextBlock::PaintRun(const FPaintContext& ctx, const Vector2& Position, const FRichTextRun& Run) const
{
    switch (Run.Type)
    {
    case ERichTextTagType::Hyperlink:
        {
            // Draw hyperlink text with underline
            ctx.Renderer->DrawTextLabel(UIRect(Position.x, Position.y, 0, Run.FontSize),
                                    Run.HyperlinkText, Run.FontSize, Run.Color,
                                    TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);

            // Draw underline
            float TextWidth = m_TextMeasurer
                ? m_TextMeasurer->Measure(Run.HyperlinkText, Run.FontSize).x
                : static_cast<float>(Run.HyperlinkText.size()) * Run.FontSize * 0.5f;

            ctx.Renderer->DrawRect(UIRect(Position.x, Position.y + Run.FontSize * 0.85f,
                                           TextWidth, 1.0f),
                                    Run.Color);
        }
        break;

    default:
        if (!Run.Text.empty())
        {
            ctx.Renderer->DrawTextLabel(UIRect(Position.x, Position.y, 0, Run.FontSize),
                                    Run.Text, Run.FontSize, Run.Color,
                                    TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);

            // Draw strikethrough if needed
            if (Run.Strikethrough)
            {
                float TextWidth = m_TextMeasurer
                    ? m_TextMeasurer->Measure(Run.Text, Run.FontSize).x
                    : static_cast<float>(Run.Text.size()) * Run.FontSize * 0.5f;

                ctx.Renderer->DrawRect(UIRect(Position.x, Position.y + Run.FontSize * 0.5f,
                                               TextWidth, 1.0f),
                                        Run.Color);
            }

            // Draw underline if needed
            if (Run.Underline && Run.Type != ERichTextTagType::Hyperlink)
            {
                float TextWidth = m_TextMeasurer
                    ? m_TextMeasurer->Measure(Run.Text, Run.FontSize).x
                    : static_cast<float>(Run.Text.size()) * Run.FontSize * 0.5f;

                ctx.Renderer->DrawRect(UIRect(Position.x, Position.y + Run.FontSize * 0.85f,
                                               TextWidth, 1.0f),
                                        Run.Color);
            }
        }
        break;
    }
}

float SRichTextBlock::MeasureRunWidth(const FRichTextRun& Run) const
{
    if (m_TextMeasurer == nullptr)
    {
        return static_cast<float>(Run.Text.size()) * Options.FontSize * 0.5f;
    }

    switch (Run.Type)
    {
    case ERichTextTagType::Hyperlink:
        return static_cast<float>(Run.HyperlinkText.size()) * Run.FontSize * 0.5f;
    default:
        return m_TextMeasurer->Measure(Run.Text, Run.FontSize).x;
    }
}

}  // namespace ZSlate
