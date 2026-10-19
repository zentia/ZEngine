#include "ZSlateAnimatorWindow.h"

#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Editor/EditorFileService/EditorFileService.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorWindow/ZSlateContentBrowserWindow/ZSlateContentBrowserWindow.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

using namespace ZSlate;

namespace
{
    const UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
    const UIColor kHeaderColor(0.78f, 0.80f, 0.86f, 1.0f);
    const UIColor kLabelColor(0.85f, 0.86f, 0.90f, 1.0f);
    const UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
    const UIColor kRowColor(0.14f, 0.14f, 0.17f, 1.0f);
    const UIColor kRowHover(0.20f, 0.20f, 0.25f, 1.0f);
    const UIColor kAccent(0.20f, 0.36f, 0.58f, 1.0f);
    const UIColor kAccentHover(0.24f, 0.42f, 0.66f, 1.0f);
    const UIColor kDanger(0.45f, 0.18f, 0.18f, 1.0f);
    const UIColor kDangerHover(0.60f, 0.24f, 0.24f, 1.0f);

    // ---- Graph canvas geometry (LOGICAL units; multiplied by the UI scale) ---
    // Persisted on AnimatorState as pos_x / pos_y, so a layout survives DPI
    // changes and a reopened controller keeps its arrangement.
    constexpr float kNodeW = 150.0f;
    // Two asset lines (clip, anim skel map) plus the title; both lines are
    // drag-out handles, so the node has to be tall enough to show them.
    constexpr float kNodeH = 74.0f;
    constexpr float kPortSize = 14.0f;
    constexpr float kLayoutColumns = 3.0f;
    constexpr float kLayoutGapX = 190.0f;
    constexpr float kLayoutGapY = 100.0f;
    constexpr float kLayoutOrigin = 24.0f;

    const char* const kParameterTypeNames[] = {"Float", "Int", "Bool", "Trigger"};
    const char* const kWrapModeNames[] = {"Loop", "Once", "Clamp", "PingPong"};
    const char* const kConditionModeNames[] = {"If", "IfNot", "Greater", "Less", "Equals", "NotEqual"};

    constexpr int kParameterTypeCount = 4;
    constexpr int kWrapModeCount = 4;
    constexpr int kConditionModeCount = 6;

    std::string FloatText(float in_value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(in_value));
        return std::string(buffer);
    }

    std::string IntText(int in_value)
    {
        return std::to_string(in_value);
    }

    /// Tolerant parse: a malformed field keeps the old value rather than writing
    /// NaN / garbage into the asset.
    float ParseFloat(const std::string& in_text, float in_fallback)
    {
        try
        {
            return std::stof(in_text);
        }
        catch (...)
        {
            return in_fallback;
        }
    }

    int ParseInt(const std::string& in_text, int in_fallback)
    {
        try
        {
            return std::stoi(in_text);
        }
        catch (...)
        {
            return in_fallback;
        }
    }

    std::string ClampName(const std::string& in_text, const std::string& in_fallback)
    {
        return in_text.empty() ? in_fallback : in_text;
    }
}  // namespace

ZSlateAnimatorWindow::ZSlateAnimatorWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kAnimator)
{
    m_Open = false;
}

// ---------------------------------------------------------------------------
// Small widget factories
// ---------------------------------------------------------------------------
std::shared_ptr<STextBlock> ZSlateAnimatorWindow::MakeLabel(const std::string& text, float font)
{
    auto label = std::make_shared<STextBlock>();
    label->Text = text;
    label->FontSize = font;
    label->Color = kLabelColor;
    label->Alignment = TextAnchor::MiddleLeft;
    return label;
}

std::shared_ptr<SButton> ZSlateAnimatorWindow::MakeButton(const std::string& label,
                                                          float font,
                                                          std::function<void()> on_click)
{
    auto button = std::make_shared<SButton>();
    button->Padding = ZSlate::FMargin(7.0f, 3.0f);
    button->NormalColor = kRowColor;
    button->HoverColor = kRowHover;
    button->SetContent(MakeLabel(label, font));
    button->OnClicked = std::move(on_click);
    return button;
}

std::shared_ptr<SEditableTextBox> ZSlateAnimatorWindow::MakeTextBox(const std::string& in_text,
                                                                    float font,
                                                                    float min_width,
                                                                    const std::string& in_hint,
                                                                    std::function<void(const std::string&)> on_commit)
{
    auto box = std::make_shared<SEditableTextBox>();
    box->Text = in_text;
    box->HintText = in_hint;
    box->FontSize = font;
    box->MinWidth = min_width;
    box->OnTextCommitted = std::move(on_commit);
    return box;
}

// ---------------------------------------------------------------------------
// Model ops
// ---------------------------------------------------------------------------
void ZSlateAnimatorWindow::AddParameter()
{
    if (m_Controller == nullptr)
    {
        return;
    }
    AnimatorParameter parameter;
    parameter.name = ("Param" + std::to_string(m_Controller->parameters.size())).c_str();
    parameter.type = static_cast<int>(AnimatorParameterType::Float);
    m_Controller->parameters.push_back(parameter);
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::RemoveParameter(int index)
{
    if (m_Controller == nullptr || index < 0 || index >= static_cast<int>(m_Controller->parameters.size()))
    {
        return;
    }
    const eastl::string removed = m_Controller->parameters[static_cast<size_t>(index)].name;
    m_Controller->parameters.erase(m_Controller->parameters.begin() + index);
    // Conditions reference parameters BY NAME, so the ones that named a deleted
    // parameter now dangle -- drop them instead of letting them silently
    // never-fire at runtime.
    for (AnimatorTransition& transition : m_Controller->transitions)
    {
        std::vector<AnimatorCondition> kept;
        for (AnimatorCondition& condition : transition.conditions)
        {
            if (condition.parameter != removed)
            {
                kept.push_back(condition);
            }
        }
        transition.conditions = kept;
    }
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::AddState()
{
    if (m_Controller == nullptr)
    {
        return;
    }
    const float slot = static_cast<float>(m_Controller->states.size());
    AnimatorState state;
    state.name = ("State" + std::to_string(m_Controller->states.size())).c_str();
    state.wrap_mode = static_cast<int>(AnimatorWrapMode::Loop);
    state.speed = 1.0f;
    state.clip_length = 1.0f;
    // Drop the node on the next free grid slot: at (0,0) it would stack on any
    // other un-arranged node, and later nodes win the hit test -- i.e. it would
    // be unclickable.
    state.pos_x = kLayoutOrigin + std::fmod(slot, kLayoutColumns) * kLayoutGapX;
    state.pos_y = kLayoutOrigin + std::floor(slot / kLayoutColumns) * kLayoutGapY;
    m_Controller->states.push_back(state);
    // Select it so the strip opens on the new node: its clip path is what has to
    // be typed next, and an unselected node has nowhere to type it.
    m_SelectedState = static_cast<int>(slot);
    m_SelectedTransition = -1;
    if (m_Controller->default_state < 0)
    {
        m_Controller->default_state = 0;
    }
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::RemoveState(int index)
{
    if (m_Controller == nullptr || index < 0 || index >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    m_Controller->states.erase(m_Controller->states.begin() + index);

    if (m_SelectedState == index)
    {
        m_SelectedState = -1;
    }
    else if (m_SelectedState > index)
    {
        --m_SelectedState;
    }
    m_DraggingNode = false;
    m_Connecting = false;
    // Transitions just got deleted / renumbered, so any arrow selection is dead.
    m_SelectedTransition = -1;
    // Same for the picker: its captured state index may have shifted under it.
    ClosePicker();

    // Transitions address states BY INDEX: drop every transition that touched
    // the removed state and renumber the rest, or they would point at the wrong
    // clip (or off the end of the array).
    std::vector<AnimatorTransition> kept;
    for (AnimatorTransition& transition : m_Controller->transitions)
    {
        if (transition.from_state == index || transition.to_state == index)
        {
            continue;
        }
        if (transition.from_state > index)
        {
            --transition.from_state;
        }
        if (transition.to_state > index)
        {
            --transition.to_state;
        }
        kept.push_back(transition);
    }
    m_Controller->transitions = kept;

    if (m_Controller->states.empty())
    {
        m_Controller->default_state = -1;
    }
    else
    {
        m_Controller->default_state = std::clamp(m_Controller->default_state, 0, static_cast<int>(m_Controller->states.size()) - 1);
    }
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::AddTransition()
{
    if (m_Controller == nullptr || m_Controller->states.size() < 2)
    {
        LOG_WARNING(ZEditor, "Animator: a transition needs at least two states");
        return;
    }
    AnimatorTransition transition;
    transition.from_state = 0;
    transition.to_state = 1;
    transition.duration = 0.25f;
    transition.priority = 0;
    m_Controller->transitions.push_back(transition);
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::RemoveTransition(int index)
{
    if (m_Controller == nullptr || index < 0 || index >= static_cast<int>(m_Controller->transitions.size()))
    {
        return;
    }
    m_Controller->transitions.erase(m_Controller->transitions.begin() + index);
    // Indices shift, so a kept selection would silently point at another arrow.
    m_SelectedTransition = -1;
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::AddCondition(int transition_index)
{
    if (m_Controller == nullptr || transition_index < 0 || transition_index >= static_cast<int>(m_Controller->transitions.size()))
    {
        return;
    }
    AnimatorCondition condition;
    condition.parameter = m_Controller->parameters.empty()
                              ? eastl::string("")
                              : m_Controller->parameters.front().name;
    condition.mode = static_cast<int>(AnimatorConditionMode::Greater);
    condition.threshold = 0.0f;
    m_Controller->transitions[static_cast<size_t>(transition_index)].conditions.push_back(condition);
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::RemoveCondition(int transition_index, int condition_index)
{
    if (m_Controller == nullptr || transition_index < 0 || transition_index >= static_cast<int>(m_Controller->transitions.size()))
    {
        return;
    }
    auto& conditions = m_Controller->transitions[static_cast<size_t>(transition_index)].conditions;
    if (condition_index < 0 || condition_index >= static_cast<int>(conditions.size()))
    {
        return;
    }
    conditions.erase(conditions.begin() + condition_index);
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::AddEvent(int state_index)
{
    if (m_Controller == nullptr || state_index < 0 || state_index >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    AnimatorEvent event;
    event.normalized_time = 0.5f;
    event.function_name = "OnEvent";
    m_Controller->states[static_cast<size_t>(state_index)].events.push_back(event);
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::RemoveEvent(int state_index, int event_index)
{
    if (m_Controller == nullptr || state_index < 0 || state_index >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    auto& events = m_Controller->states[static_cast<size_t>(state_index)].events;
    if (event_index < 0 || event_index >= static_cast<int>(events.size()))
    {
        return;
    }
    events.erase(events.begin() + event_index);
    m_UiDirty = true;
    m_ModelDirty = true;
}

// ---------------------------------------------------------------------------
// Table builders
// ---------------------------------------------------------------------------
void ZSlateAnimatorWindow::BuildParameterRows(const std::shared_ptr<SScrollBox>& in_box, float scale)
{
    const float font = 13.0f * scale;
    const float field_w = 120.0f * scale;

    for (size_t index = 0; index < m_Controller->parameters.size(); ++index)
    {
        AnimatorParameter& parameter = m_Controller->parameters[index];
        const int captured = static_cast<int>(index);

        auto row = std::make_shared<SHorizontalBox>();
        auto pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
            return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 4.0f, 0.0f));
        };

        // Type cycler (Float -> Int -> Bool -> Trigger).
        const int type_index = std::clamp(parameter.type, 0, kParameterTypeCount - 1);
        auto type_button = MakeButton(kParameterTypeNames[type_index], font, [this, captured]() {
            AnimatorParameter& target = m_Controller->parameters[static_cast<size_t>(captured)];
            target.type = (target.type + 1) % kParameterTypeCount;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        type_button->NormalColor = kAccent;
        type_button->HoverColor = kAccentHover;
        pad(row->AddSlot(type_button));

        pad(row->AddSlot(MakeTextBox(parameter.name.c_str(), font, field_w, "name", [this, captured](const std::string& t) {
            m_Controller->parameters[static_cast<size_t>(captured)].name = ClampName(t, "Param").c_str();
            m_ModelDirty = true;
        })));

        // The default editor follows the parameter's type.
        const AnimatorParameterType type = static_cast<AnimatorParameterType>(parameter.type);
        std::string default_text;
        std::function<void(const std::string&)> on_commit;
        switch (type)
        {
            case AnimatorParameterType::Float:
                default_text = FloatText(parameter.default_float);
                on_commit = [this, captured](const std::string& t) {
                    m_Controller->parameters[static_cast<size_t>(captured)].default_float = ParseFloat(t, 0.0f);
                    m_ModelDirty = true;
                };
                break;
            case AnimatorParameterType::Int:
                default_text = IntText(parameter.default_int);
                on_commit = [this, captured](const std::string& t) {
                    m_Controller->parameters[static_cast<size_t>(captured)].default_int = ParseInt(t, 0);
                    m_ModelDirty = true;
                };
                break;
            case AnimatorParameterType::Bool:
                default_text = parameter.default_bool ? "true" : "false";
                on_commit = [this, captured](const std::string& t) {
                    m_Controller->parameters[static_cast<size_t>(captured)].default_bool = (t == "true" || t == "1");
                    m_ModelDirty = true;
                };
                break;
            case AnimatorParameterType::Trigger:
                default_text = "(trigger)";
                on_commit = [](const std::string&) {};
                break;
        }
        pad(row->AddSlot(MakeTextBox(default_text, font, 80.0f * scale, "default", std::move(on_commit))));

        auto remove = MakeButton("X", font, [this, captured]() { RemoveParameter(captured); });
        remove->NormalColor = kDanger;
        remove->HoverColor = kDangerHover;
        pad(row->AddSlot(remove));

        in_box->AddChild(row);
    }

    if (m_Controller->parameters.empty())
    {
        in_box->AddChild(MakeLabel("(no parameters)", font));
    }
}

// One state's editable rows. Shared by the States tab and by the in-canvas
// strip, so the two cannot drift apart.
void ZSlateAnimatorWindow::BuildStateBody(const std::shared_ptr<SScrollBox>& in_box, float scale, int captured)
{
    const float font = 13.0f * scale;
    const float field_w = 150.0f * scale;
    if (m_Controller == nullptr || captured < 0 || captured >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    {
        AnimatorState& state = m_Controller->states[static_cast<size_t>(captured)];

        auto header = std::make_shared<SHorizontalBox>();
        auto pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
            return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 4.0f, 0.0f));
        };

        // Row 1: index | default marker | name | wrap cycler | delete
        char index_text[32];
        std::snprintf(index_text, sizeof(index_text), "#%d", captured);
        pad(header->AddSlot(MakeLabel(index_text, font)));

        auto default_button = MakeButton(m_Controller->default_state == captured ? "[*]" : "[ ]", font, [this, captured]() {
            m_Controller->default_state = captured;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        pad(header->AddSlot(default_button));

        pad(header->AddSlot(MakeTextBox(state.name.c_str(), font, field_w, "state name", [this, captured](const std::string& t) {
            m_Controller->states[static_cast<size_t>(captured)].name = ClampName(t, "State").c_str();
            m_ModelDirty = true;
        })));

        const int wrap_index = std::clamp(state.wrap_mode, 0, kWrapModeCount - 1);
        auto wrap_button = MakeButton(kWrapModeNames[wrap_index], font, [this, captured]() {
            AnimatorState& target = m_Controller->states[static_cast<size_t>(captured)];
            target.wrap_mode = (target.wrap_mode + 1) % kWrapModeCount;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        wrap_button->NormalColor = kAccent;
        wrap_button->HoverColor = kAccentHover;
        pad(header->AddSlot(wrap_button));

        auto remove = MakeButton("X", font, [this, captured]() { RemoveState(captured); });
        remove->NormalColor = kDanger;
        remove->HoverColor = kDangerHover;
        pad(header->AddSlot(remove));

        in_box->AddChild(header);

        // Row 2: clip path. Also a drag SOURCE: the row can be dragged onto any
        // asset path field (another window's component, the Content Browser...).
        auto clip_box = MakeTextBox(state.clip_file_path.c_str(), font, 220.0f * scale, "clip .zasset path",
                                    [this, captured](const std::string& t) {
                                        m_Controller->states[static_cast<size_t>(captured)].clip_file_path = t.c_str();
                                        m_ModelDirty = true;
                                    });
        in_box->AddChild(EditorDragDrop::MakeAssetPathDragSource(clip_box));

        // Row 3: anim->skeleton map | speed | length (seconds)
        auto row3 = std::make_shared<SHorizontalBox>();
        auto map_box = MakeTextBox(state.anim_skel_map_path.c_str(), font, 150.0f * scale, "anim skel map",
                                   [this, captured](const std::string& t) {
                                       m_Controller->states[static_cast<size_t>(captured)].anim_skel_map_path = t.c_str();
                                       m_ModelDirty = true;
                                   });
        pad(row3->AddSlot(EditorDragDrop::MakeAssetPathDragSource(map_box)));
        auto map_browse = MakeButton("Browse", font, [this, captured]() { OpenPicker(PickField::SkelMap, captured); });
        map_browse->NormalColor = kAccent;
        map_browse->HoverColor = kAccentHover;
        pad(row3->AddSlot(map_browse));
        pad(row3->AddSlot(MakeTextBox(FloatText(state.speed), font, 60.0f * scale, "speed", [this, captured](const std::string& t) {
            m_Controller->states[static_cast<size_t>(captured)].speed = ParseFloat(t, 1.0f);
            m_ModelDirty = true;
        })));
        pad(row3->AddSlot(MakeTextBox(FloatText(state.clip_length), font, 70.0f * scale, "len(s)",
                                      [this, captured](const std::string& t) {
                                          m_Controller->states[static_cast<size_t>(captured)].clip_length = ParseFloat(t, 1.0f);
                                          m_ModelDirty = true;
                                      })));
        in_box->AddChild(row3);

        // Row 4+: events
        for (size_t event_index = 0; event_index < state.events.size(); ++event_index)
        {
            AnimatorEvent& anim_event = state.events[event_index];
            const int event_captured = static_cast<int>(event_index);
            auto event_row = std::make_shared<SHorizontalBox>();
            pad(event_row->AddSlot(MakeLabel("event", font)));
            pad(event_row->AddSlot(MakeTextBox(FloatText(anim_event.normalized_time), font, 60.0f * scale, "t",
                                               [this, captured, event_captured](const std::string& t) {
                                                   m_Controller->states[static_cast<size_t>(captured)]
                                                       .events[static_cast<size_t>(event_captured)]
                                                       .normalized_time = std::clamp(ParseFloat(t, 0.0f), 0.0f, 1.0f);
                                                   m_ModelDirty = true;
                                               })));
            pad(event_row->AddSlot(MakeTextBox(anim_event.function_name.c_str(), font, 120.0f * scale, "function",
                                               [this, captured, event_captured](const std::string& t) {
                                                   m_Controller->states[static_cast<size_t>(captured)]
                                                       .events[static_cast<size_t>(event_captured)]
                                                       .function_name = t.c_str();
                                                   m_ModelDirty = true;
                                               })));
            auto remove_event = MakeButton("x", font, [this, captured, event_captured]() { RemoveEvent(captured, event_captured); });
            remove_event->NormalColor = kDanger;
            remove_event->HoverColor = kDangerHover;
            pad(event_row->AddSlot(remove_event));
            in_box->AddChild(event_row);
        }
        auto add_event = MakeButton("+ Event", font, [this, captured]() { AddEvent(captured); });
        in_box->AddChild(add_event);
    }
}

void ZSlateAnimatorWindow::BuildStateRows(const std::shared_ptr<SScrollBox>& in_box, float scale)
{
    for (size_t index = 0; index < m_Controller->states.size(); ++index)
    {
        BuildStateBody(in_box, scale, static_cast<int>(index));

        // Separator between states.
        in_box->AddChild(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f, 6.0f * scale)));
    }

    if (m_Controller->states.empty())
    {
        in_box->AddChild(MakeLabel("(no states)", 13.0f * scale));
    }
}

// One transition's editable rows. Shared by the Transitions tab and by the
// in-canvas editor strip, so the two cannot drift apart.
void ZSlateAnimatorWindow::BuildTransitionBody(const std::shared_ptr<SScrollBox>& in_box, float scale, int captured)
{
    const float font = 13.0f * scale;
    if (m_Controller == nullptr || captured < 0 || captured >= static_cast<int>(m_Controller->transitions.size()))
    {
        return;
    }
    {
        AnimatorTransition& transition = m_Controller->transitions[static_cast<size_t>(captured)];

        auto pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
            return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 4.0f, 0.0f));
        };

        // State name helper: transitions store indices, the UI shows names.
        auto state_label = [this](int state_index) -> std::string {
            if (state_index < 0 || state_index >= static_cast<int>(m_Controller->states.size()))
            {
                return "?";
            }
            return m_Controller->states[static_cast<size_t>(state_index)].name.c_str();
        };
        // Row 1: from -> to | delete
        auto row1 = std::make_shared<SHorizontalBox>();
        const int state_count = static_cast<int>(m_Controller->states.size());

        auto from_button = MakeButton(state_label(transition.from_state), font, [this, captured, state_count]() {
            AnimatorTransition& target = m_Controller->transitions[static_cast<size_t>(captured)];
            target.from_state = state_count > 0 ? (target.from_state + 1) % state_count : 0;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        from_button->NormalColor = kAccent;
        from_button->HoverColor = kAccentHover;
        pad(row1->AddSlot(from_button));
        pad(row1->AddSlot(MakeLabel("->", font)));

        auto to_button = MakeButton(state_label(transition.to_state), font, [this, captured, state_count]() {
            AnimatorTransition& target = m_Controller->transitions[static_cast<size_t>(captured)];
            target.to_state = state_count > 0 ? (target.to_state + 1) % state_count : 0;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        to_button->NormalColor = kAccent;
        to_button->HoverColor = kAccentHover;
        pad(row1->AddSlot(to_button));

        auto remove = MakeButton("X", font, [this, captured]() { RemoveTransition(captured); });
        remove->NormalColor = kDanger;
        remove->HoverColor = kDangerHover;
        pad(row1->AddSlot(remove));
        in_box->AddChild(row1);

        // Row 2: duration | exit time | priority
        auto row2 = std::make_shared<SHorizontalBox>();
        pad(row2->AddSlot(MakeTextBox(FloatText(transition.duration), font, 60.0f * scale, "fade(s)",
                                      [this, captured](const std::string& t) {
                                          m_Controller->transitions[static_cast<size_t>(captured)].duration =
                                              std::max(0.0f, ParseFloat(t, 0.0f));
                                          m_ModelDirty = true;
                                      })));

        auto exit_check = std::make_shared<SCheckBox>();
        exit_check->Checked = transition.has_exit_time;
        exit_check->BoxSize = 14.0f * scale;
        exit_check->OnCheckStateChanged = [this, captured](bool checked) {
            m_Controller->transitions[static_cast<size_t>(captured)].has_exit_time = checked;
            m_ModelDirty = true;
        };
        pad(row2->AddSlot(exit_check));
        pad(row2->AddSlot(MakeLabel("exit", font)));
        pad(row2->AddSlot(MakeTextBox(FloatText(transition.exit_time), font, 55.0f * scale, "0-1",
                                      [this, captured](const std::string& t) {
                                          m_Controller->transitions[static_cast<size_t>(captured)].exit_time =
                                              std::clamp(ParseFloat(t, 0.0f), 0.0f, 1.0f);
                                          m_ModelDirty = true;
                                      })));
        pad(row2->AddSlot(MakeLabel("pri", font)));
        pad(row2->AddSlot(MakeTextBox(IntText(transition.priority), font, 45.0f * scale, "0",
                                      [this, captured](const std::string& t) {
                                          m_Controller->transitions[static_cast<size_t>(captured)].priority = ParseInt(t, 0);
                                          m_ModelDirty = true;
                                      })));
        in_box->AddChild(row2);

        // Row 3+: conditions
        for (size_t condition_index = 0; condition_index < transition.conditions.size(); ++condition_index)
        {
            AnimatorCondition& condition = transition.conditions[condition_index];
            const int condition_captured = static_cast<int>(condition_index);

            auto condition_row = std::make_shared<SHorizontalBox>();
            const int parameter_count = static_cast<int>(m_Controller->parameters.size());

            auto parameter_button = MakeButton(condition.parameter.empty() ? "(param)" : condition.parameter.c_str(), font,
                                               [this, captured, condition_captured, parameter_count]() {
                                                   if (parameter_count == 0)
                                                   {
                                                       return;
                                                   }
                                                   AnimatorCondition& target = m_Controller->transitions[static_cast<size_t>(captured)]
                                                                                   .conditions[static_cast<size_t>(condition_captured)];
                                                   const int current = m_Controller->FindParameter(target.parameter.c_str());
                                                   const int next = (current + 1 + parameter_count) % parameter_count;
                                                   target.parameter = m_Controller->parameters[static_cast<size_t>(next)].name;
                                                   m_UiDirty = true;
                                                   m_ModelDirty = true;
                                               });
            pad(condition_row->AddSlot(parameter_button));

            const int mode_index = std::clamp(condition.mode, 0, kConditionModeCount - 1);
            auto mode_button = MakeButton(kConditionModeNames[mode_index], font,
                                          [this, captured, condition_captured]() {
                                              AnimatorCondition& target = m_Controller->transitions[static_cast<size_t>(captured)]
                                                                              .conditions[static_cast<size_t>(condition_captured)];
                                              target.mode = (target.mode + 1) % kConditionModeCount;
                                              m_UiDirty = true;
                                              m_ModelDirty = true;
                                          });
            pad(condition_row->AddSlot(mode_button));

            pad(condition_row->AddSlot(MakeTextBox(FloatText(condition.threshold), font, 60.0f * scale, "value",
                                                   [this, captured, condition_captured](const std::string& t) {
                                                       m_Controller->transitions[static_cast<size_t>(captured)]
                                                           .conditions[static_cast<size_t>(condition_captured)]
                                                           .threshold = ParseFloat(t, 0.0f);
                                                       m_ModelDirty = true;
                                                   })));

            auto remove_condition = MakeButton("x", font,
                                               [this, captured, condition_captured]() { RemoveCondition(captured, condition_captured); });
            remove_condition->NormalColor = kDanger;
            remove_condition->HoverColor = kDangerHover;
            pad(condition_row->AddSlot(remove_condition));
            in_box->AddChild(condition_row);
        }

        auto add_condition = MakeButton("+ Condition", font, [this, captured]() { AddCondition(captured); });
        in_box->AddChild(add_condition);
    }
}

void ZSlateAnimatorWindow::BuildTransitionRows(const std::shared_ptr<SScrollBox>& in_box, float scale)
{
    for (size_t index = 0; index < m_Controller->transitions.size(); ++index)
    {
        BuildTransitionBody(in_box, scale, static_cast<int>(index));
        in_box->AddChild(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f, 6.0f * scale)));
    }

    if (m_Controller->transitions.empty())
    {
        in_box->AddChild(MakeLabel("(no transitions)", 13.0f * scale));
    }
}

// ---------------------------------------------------------------------------
// Clip / anim-skel-map picker
// ---------------------------------------------------------------------------
namespace
{
    /// Paths travel through the controller in content-root-relative form (the
    /// same form `AssetManager::loadAsset` resolves), so a picked asset keeps
    /// working when the project moves.
    std::string ToContentRelative(const std::string& in_path)
    {
        auto* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return in_path;
        }
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(in_path), ec).lexically_normal();
        const std::filesystem::path content = std::filesystem::absolute(project_info->GetProjectContent(), ec).lexically_normal();
        if (content.empty())
        {
            return absolute.generic_string();
        }
        const std::filesystem::path relative = absolute.lexically_relative(content);
        const std::string relative_str = relative.generic_string();
        if (relative_str.empty() || relative_str.rfind("..", 0) == 0)
        {
            return absolute.generic_string();
        }
        return relative_str;
    }

    std::string LowerCopy(const std::string& in_text)
    {
        std::string out = in_text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }
}  // namespace

void ZSlateAnimatorWindow::OpenPicker(PickField field, int state_index)
{
    if (m_Controller == nullptr || field == PickField::None)
    {
        return;
    }
    m_PickField = field;
    m_PickStateIndex = state_index;
    m_PickFilter.clear();
    RefreshPickCandidates();
    m_UiDirty = true;
}

void ZSlateAnimatorWindow::ClosePicker()
{
    m_PickField = PickField::None;
    m_PickStateIndex = -1;
    m_PickFilter.clear();
    m_PickRows.clear();
    m_UiDirty = true;
}

void ZSlateAnimatorWindow::RefreshPickCandidates()
{
    m_PickCandidates.clear();
    m_PickRows.clear();

    const std::string wanted = (m_PickField == PickField::Clip) ? "animationasset" : "animskelmap";

    // Collect every .zasset in the project; the typed ones go in first.
    std::vector<std::string> all_zassets;
    std::vector<std::string> matched;

    auto consider = [&](const std::string& path, const std::string& asset_type) {
        const std::string relative = ToContentRelative(path);
        all_zassets.push_back(relative);
        if (LowerCopy(asset_type) == wanted)
        {
            matched.push_back(relative);
        }
    };

    auto* browser = ZSlateContentBrowserWindow::GetInstance();
    if (browser != nullptr && browser->GetFileService().getEditorRootNode() != nullptr)
    {
        EditorFileService& service = browser->GetFileService();
        // Deliberately NOT calling BuildEngineFileTree() here: it clears the
        // node array the Content Browser is holding raw pointers into
        // (m_SelectedNode / m_BrowsedFolderNode), which would leave it dangling.
        // Rebuilding is the browser's job (it rebinds its selection afterwards);
        // while its tree is empty we fall back to scanning disk instead.
        std::function<void(EditorFileNode*)> walk = [&](EditorFileNode* node) {
            if (node == nullptr)
            {
                return;
            }
            if (node->isFolder())
            {
                for (const std::shared_ptr<EditorFileNode>& child : node->m_ChildNodes)
                {
                    walk(child.get());
                }
                return;
            }
            if (node->m_FileExtension == "zasset")
            {
                consider(node->m_FilePath.c_str(), node->m_AssetType);
            }
        };
        for (EditorFileNode* root : service.getEditorRootNodes())
        {
            walk(root);
        }
    }
    else if (auto* project_info = GET_SYSTEM(ProjectInfo))
    {
        // No browser tree to walk (window not created yet, or not built yet):
        // scan the content root directly rather than leaving the picker empty.
        // Without the tree's m_AssetType this lists every .zasset.
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(project_info->GetProjectContent(), ec), end; it != end; ++it)
        {
            if (it->is_regular_file() && it->path().extension().generic_string() == ".zasset")
            {
                consider(it->path().generic_string(), std::string());
            }
        }
    }

    m_PickCandidates = matched.empty() ? all_zassets : matched;
    std::sort(m_PickCandidates.begin(), m_PickCandidates.end());
    m_PickCandidates.erase(std::unique(m_PickCandidates.begin(), m_PickCandidates.end()), m_PickCandidates.end());
}

void ZSlateAnimatorWindow::ApplyPick(const std::string& content_relative_path)
{
    if (m_Controller == nullptr || m_PickStateIndex < 0 || m_PickStateIndex >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    AnimatorState& state = m_Controller->states[static_cast<size_t>(m_PickStateIndex)];
    if (m_PickField == PickField::SkelMap)
    {
        state.anim_skel_map_path = content_relative_path.c_str();
    }
    else
    {
        state.clip_file_path = content_relative_path.c_str();
    }
    m_ModelDirty = true;
    ClosePicker();
}

// ---------------------------------------------------------------------------
// Cross-window drop target: Content Browser asset -> canvas
// ---------------------------------------------------------------------------
std::string ZSlateAnimatorWindow::NormalizeAssetType(const std::string& path)
{
    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr || path.empty())
    {
        return {};
    }
    std::string type = asset_manager->GetAssetTypeName(path);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // "AnimationAssetRes" and "AnimationAsset" must compare equal; the ZAsset
    // header sometimes carries the Resource wrapper class name.
    const std::string res_suffix = "res";
    if (type.size() > res_suffix.size() && type.compare(type.size() - res_suffix.size(), res_suffix.size(), res_suffix) == 0)
    {
        type.erase(type.size() - res_suffix.size());
    }
    return type;
}

void ZSlateAnimatorWindow::ApplyAssetDrop(const ZSlate::Vector2& mouse)
{
    const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(m_PendingDragOp);
    if (m_Controller == nullptr || asset_op == nullptr)
    {
        return;
    }

    const std::string stored_path = EditorDragDrop::MakeDroppedAssetStoredPath(asset_op->AssetPath);
    const std::string type = NormalizeAssetType(asset_op->AssetPath);
    const int hit = HitNode(m_GraphRect, mouse);

    if (hit < 0)
    {
        // Blank canvas: the only meaningful drop is another controller to open.
        if (type == "animatorcontroller")
        {
            std::string error;
            OpenController(asset_op->AssetPath, error);
        }
        return;
    }

    AnimatorState& state = m_Controller->states[static_cast<size_t>(hit)];
    if (type == "animationasset")
    {
        state.clip_file_path = stored_path.c_str();
    }
    else if (type == "animskelmap")
    {
        state.anim_skel_map_path = stored_path.c_str();
    }
    else
    {
        // Anything else (mesh, material, texture...) is silently ignored: the
        // per-frame drop label already told the user what was coming.
        return;
    }

    // Select the node so the strip opens on it -- the drop is an edit.
    m_SelectedState = hit;
    m_SelectedTransition = -1;
    m_UiDirty = true;
    m_ModelDirty = true;
}

void ZSlateAnimatorWindow::UpdatePickFilter()
{
    if (m_PickField == PickField::None)
    {
        return;
    }
    const std::string needle = LowerCopy(m_PickFilter);
    for (std::pair<std::shared_ptr<ZSlate::SWidget>, std::string>& row : m_PickRows)
    {
        const bool match = needle.empty() || row.second.find(needle) != std::string::npos;
        row.first->Visibility = match ? EVisibility::Visible : EVisibility::Collapsed;
    }
}

void ZSlateAnimatorWindow::BuildPickerPanel(const std::shared_ptr<SVerticalBox>& in_parent, float scale)
{
    const float font = 13.0f * scale;

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = kPanelColor;
    border->Padding = ZSlate::FMargin(6.0f, 5.0f, 6.0f, 5.0f);
    auto column = std::make_shared<SVerticalBox>();

    auto header = std::make_shared<SHorizontalBox>();
    auto header_pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
        return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 5.0f, 0.0f));
    };

    char title[200];
    const char* what = (m_PickField == PickField::SkelMap) ? "anim->skeleton map" : "clip";
    const char* state_name = (m_PickStateIndex >= 0 && m_PickStateIndex < static_cast<int>(m_Controller->states.size()))
                                 ? m_Controller->states[static_cast<size_t>(m_PickStateIndex)].name.c_str()
                                 : "?";
    std::snprintf(title, sizeof(title), "Pick %s for '%s' (%zu)", what, state_name, m_PickCandidates.size());
    auto title_label = MakeLabel(title, font);
    title_label->Color = kAccentHover;
    header_pad(header->AddSlot(title_label));

    auto cancel = MakeButton("Cancel", font, [this]() { ClosePicker(); });
    cancel->NormalColor = kDanger;
    cancel->HoverColor = kDangerHover;
    header_pad(header->AddSlot(cancel));
    column->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 5.0f));

    // Deliberately no m_UiDirty on commit: rows are filtered by Visibility each
    // frame instead, so typing here keeps keyboard focus.
    column->AddSlot(MakeTextBox(m_PickFilter, font, 260.0f * scale, "filter", [this](const std::string& t) { m_PickFilter = t; }))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    auto scroll = std::make_shared<SScrollBox>();
    m_PickRows.clear();
    for (const std::string& candidate : m_PickCandidates)
    {
        const std::string path = candidate;
        auto pick = MakeButton(path, font, [this, path]() { ApplyPick(path); });
        scroll->AddChild(pick);
        m_PickRows.emplace_back(pick, LowerCopy(path));
    }
    if (m_PickCandidates.empty())
    {
        scroll->AddChild(MakeLabel("(no .zasset found -- import an animation first)", font));
    }

    column->AddSlot(scroll).Fill(1.0f);

    border->SetContent(column);
    in_parent->AddSlot(border).Fill(1.0f).SetPadding(ZSlate::FMargin(0.0f, 4.0f, 0.0f, 0.0f));
}

// The strip under the canvas: the selected NODE's own rows, so a state can be
// edited without leaving the graph.
void ZSlateAnimatorWindow::BuildSelectedStateEditor(const std::shared_ptr<SVerticalBox>& in_parent, float scale)
{
    const float font = 13.0f * scale;
    if (m_Controller == nullptr || m_SelectedState < 0 || m_SelectedState >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }

    const AnimatorState& state = m_Controller->states[static_cast<size_t>(m_SelectedState)];

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = kPanelColor;
    border->Padding = ZSlate::FMargin(6.0f, 5.0f, 6.0f, 5.0f);
    auto column = std::make_shared<SVerticalBox>();

    auto header = std::make_shared<SHorizontalBox>();
    auto header_pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
        return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 5.0f, 0.0f));
    };

    char title[160];
    std::snprintf(title, sizeof(title), "State: %s", state.name.c_str());
    auto title_label = MakeLabel(title, font);
    title_label->Color = kAccentHover;
    header_pad(header->AddSlot(title_label));

    // Default marker: the canvas draws it as "*", so the strip must say so too.
    if (m_Controller->default_state == m_SelectedState)
    {
        auto default_label = MakeLabel("[*] default", font);
        default_label->Color = kAccent;
        header_pad(header->AddSlot(default_label));
    }
    else
    {
        auto default_button = MakeButton("Set Default", font, [this]() {
            m_Controller->default_state = m_SelectedState;
            m_UiDirty = true;
            m_ModelDirty = true;
        });
        default_button->NormalColor = kAccent;
        default_button->HoverColor = kAccentHover;
        header_pad(header->AddSlot(default_button));
    }

    // Deleting a state renumbers every transition, so it stays an explicit
    // button here rather than a right-click gesture on the node.
    auto delete_button = MakeButton("Delete", font, [this]() { RemoveState(m_SelectedState); });
    delete_button->NormalColor = kDanger;
    delete_button->HoverColor = kDangerHover;
    header_pad(header->AddSlot(delete_button));

    auto close_button = MakeButton("Deselect", font, [this]() {
        m_SelectedState = -1;
        m_UiDirty = true;
    });
    header_pad(header->AddSlot(close_button));
    column->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    auto scroll = std::make_shared<SScrollBox>();
    BuildStateBody(scroll, scale, m_SelectedState);
    // Capped height: a state with many events must not swallow the canvas.
    auto cap = std::make_shared<SBox>();
    cap->HeightOverride = 190.0f * scale;
    cap->SetContent(scroll);
    column->AddSlot(cap).AutoSize();

    border->SetContent(column);
    in_parent->AddSlot(border).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 4.0f, 0.0f, 0.0f));
}

// The strip under the canvas: the selected transition's own rows, so an arrow
// can be edited without leaving the graph.
void ZSlateAnimatorWindow::BuildSelectedTransitionEditor(const std::shared_ptr<SVerticalBox>& in_parent, float scale)
{
    const float font = 13.0f * scale;
    if (m_Controller == nullptr || m_SelectedTransition < 0 ||
        m_SelectedTransition >= static_cast<int>(m_Controller->transitions.size()))
    {
        return;
    }

    const AnimatorTransition& transition = m_Controller->transitions[static_cast<size_t>(m_SelectedTransition)];

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = kPanelColor;
    border->Padding = ZSlate::FMargin(6.0f, 5.0f, 6.0f, 5.0f);
    auto column = std::make_shared<SVerticalBox>();

    auto header = std::make_shared<SHorizontalBox>();
    auto header_pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
        return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 5.0f, 0.0f));
    };

    char title[160];
    const char* from_name = (transition.from_state >= 0 && transition.from_state < static_cast<int>(m_Controller->states.size()))
                                ? m_Controller->states[static_cast<size_t>(transition.from_state)].name.c_str()
                                : "?";
    const char* to_name = (transition.to_state >= 0 && transition.to_state < static_cast<int>(m_Controller->states.size()))
                              ? m_Controller->states[static_cast<size_t>(transition.to_state)].name.c_str()
                              : "?";
    std::snprintf(title, sizeof(title), "Transition: %s -> %s", from_name, to_name);
    auto title_label = MakeLabel(title, font);
    title_label->Color = kAccentHover;
    header_pad(header->AddSlot(title_label));

    auto delete_button = MakeButton("Delete (or right-click the arrow)", font, [this]() {
        if (m_SelectedTransition >= 0)
        {
            RemoveTransition(m_SelectedTransition);
        }
    });
    delete_button->NormalColor = kDanger;
    delete_button->HoverColor = kDangerHover;
    header_pad(header->AddSlot(delete_button));

    auto close_button = MakeButton("Deselect", font, [this]() {
        m_SelectedTransition = -1;
        m_UiDirty = true;
    });
    header_pad(header->AddSlot(close_button));
    column->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    auto scroll = std::make_shared<SScrollBox>();
    BuildTransitionBody(scroll, scale, m_SelectedTransition);
    // Capped height: a transition with many conditions must not swallow the
    // canvas above it.
    auto cap = std::make_shared<SBox>();
    cap->HeightOverride = 150.0f * scale;
    cap->SetContent(scroll);
    column->AddSlot(cap).AutoSize();

    border->SetContent(column);
    in_parent->AddSlot(border).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 4.0f, 0.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Graph canvas
// ---------------------------------------------------------------------------
// Node geometry is stored in LOGICAL units (unscaled) on the controller and
// multiplied by the UI scale at paint time, so a layout survives a DPI change
// and a drag converts the pointer delta back through the same factor.
namespace
{
    bool RectContains(const ZSlate::UIRect& rect, const ZSlate::Vector2& point)
    {
        return point.x >= rect.x && point.x <= rect.Right() && point.y >= rect.y && point.y <= rect.Bottom();
    }

    /// The two asset lines of a node, in draw order. Both are drag-out handles.
    enum class AssetSlot
    {
        Clip = 0,
        SkelMap = 1
    };

    /// The stored path a line shows. Painting, hit-testing and drag-out all read
    /// it from here, so the handle you see always carries what it claims to.
    const char* AssetSlotPath(const AnimatorState& state, AssetSlot slot)
    {
        return slot == AssetSlot::Clip ? state.clip_file_path.c_str() : state.anim_skel_map_path.c_str();
    }

    /// Geometry of one asset line. Painting and hit-testing share it (one
    /// function, both callers) -- a handle drawn in one place but hit-tested in
    /// another is the classic way to make a drag do nothing.
    ZSlate::UIRect AssetChipRect(const ZSlate::UIRect& region, float scale, const AnimatorState& state, AssetSlot slot)
    {
        // Title occupies the top ~21px; the two lines stack below it and leave
        // room for the connection port at the bottom.
        const float row = (slot == AssetSlot::Clip) ? 22.0f : 40.0f;
        return ZSlate::UIRect(region.x + (state.pos_x + 6.0f) * scale,
                              region.y + (state.pos_y + row) * scale,
                              (kNodeW - 12.0f) * scale,
                              18.0f * scale);
    }

    /// Point-to-segment distance: an arrow is hit-tested as a fat segment rather
    /// than as a rect, because it can run at any angle.
    float DistanceToSegment(const ZSlate::Vector2& p, const ZSlate::Vector2& a, const ZSlate::Vector2& b)
    {
        const ZSlate::Vector2 ab {b.x - a.x, b.y - a.y};
        const float length_sq = ab.x * ab.x + ab.y * ab.y;
        float t = 0.0f;
        if (length_sq > 1.0e-6f)
        {
            t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / length_sq, 0.0f, 1.0f);
        }
        const float dx = p.x - (a.x + ab.x * t);
        const float dy = p.y - (a.y + ab.y * t);
        return std::sqrt(dx * dx + dy * dy);
    }

    /// Axis-aligned "thick line" as a convex polygon: the renderer has no
    /// DrawLine, so a shaft is a rotated rectangle (4 points).
    void DrawArrow(ZSlate::ISlateRenderer& renderer,
                   const ZSlate::Vector2& from,
                   const ZSlate::Vector2& to,
                   float thickness,
                   const UIColor& color)
    {
        ZSlate::Vector2 dir {to.x - from.x, to.y - from.y};
        const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (length < 1.0e-3f)
        {
            return;
        }
        dir.x /= length;
        dir.y /= length;
        const ZSlate::Vector2 normal {-dir.y, dir.x};
        const float half = thickness * 0.5f;

        const ZSlate::Vector2 points[4] = {
            {from.x + normal.x * half, from.y + normal.y * half},
            {to.x + normal.x * half, to.y + normal.y * half},
            {to.x - normal.x * half, to.y - normal.y * half},
            {from.x - normal.x * half, from.y - normal.y * half},
        };
        renderer.DrawConvexPoly(points, 4, color);

        // Arrowhead: a triangle just short of the target.
        const float head = thickness * 2.6f;
        const ZSlate::Vector2 base {to.x - dir.x * head, to.y - dir.y * head};
        const ZSlate::Vector2 head_points[3] = {
            to,
            {base.x + normal.x * head * 0.6f, base.y + normal.y * head * 0.6f},
            {base.x - normal.x * head * 0.6f, base.y - normal.y * head * 0.6f},
        };
        renderer.DrawConvexPoly(head_points, 3, color);
    }
}  // namespace

void ZSlateAnimatorWindow::AutoLayout()
{
    if (m_Controller == nullptr)
    {
        return;
    }
    for (size_t i = 0; i < m_Controller->states.size(); ++i)
    {
        const float column = std::fmod(static_cast<float>(i), kLayoutColumns);
        const float row = std::floor(static_cast<float>(i) / kLayoutColumns);
        m_Controller->states[i].pos_x = kLayoutOrigin + column * kLayoutGapX;
        m_Controller->states[i].pos_y = kLayoutOrigin + row * kLayoutGapY;
    }
    m_ModelDirty = true;
}

int ZSlateAnimatorWindow::HitNode(const ZSlate::UIRect& region, const ZSlate::Vector2& point) const
{
    if (m_Controller == nullptr)
    {
        return -1;
    }
    const float scale = m_BuiltScale > 0.5f ? m_BuiltScale : 1.0f;
    const float local_x = (point.x - region.x) / scale;
    const float local_y = (point.y - region.y) / scale;

    // Topmost first: the tree is rebuilt in order, so later nodes paint over
    // earlier ones and should win the hit test too.
    for (int i = static_cast<int>(m_Controller->states.size()) - 1; i >= 0; --i)
    {
        const AnimatorState& state = m_Controller->states[static_cast<size_t>(i)];
        if (local_x >= state.pos_x && local_x <= state.pos_x + kNodeW && local_y >= state.pos_y &&
            local_y <= state.pos_y + kNodeH)
        {
            return i;
        }
    }
    return -1;
}

bool ZSlateAnimatorWindow::GetTransitionEndpoints(const ZSlate::UIRect& region,
                                                  float scale,
                                                  const AnimatorTransition& transition,
                                                  ZSlate::Vector2& out_from,
                                                  ZSlate::Vector2& out_to) const
{
    if (m_Controller == nullptr)
    {
        return false;
    }
    const int state_count = static_cast<int>(m_Controller->states.size());
    if (transition.from_state < 0 || transition.from_state >= state_count || transition.to_state < 0 ||
        transition.to_state >= state_count)
    {
        return false;
    }

    const AnimatorState& from = m_Controller->states[static_cast<size_t>(transition.from_state)];
    const AnimatorState& to = m_Controller->states[static_cast<size_t>(transition.to_state)];

    if (transition.from_state == transition.to_state)
    {
        // A self-transition has no room for a real arrow: stub it just below the
        // node so it stays visible and clickable.
        out_from = ZSlate::Vector2(region.x + (from.pos_x + kNodeW * 0.5f - 16.0f) * scale,
                                   region.y + (from.pos_y + kNodeH + 12.0f) * scale);
        out_to = ZSlate::Vector2(region.x + (from.pos_x + kNodeW * 0.5f + 16.0f) * scale,
                                 region.y + (from.pos_y + kNodeH + 12.0f) * scale);
        return true;
    }

    out_from = ZSlate::Vector2(region.x + (from.pos_x + kNodeW * 0.5f) * scale, region.y + (from.pos_y + kNodeH) * scale);
    out_to = ZSlate::Vector2(region.x + (to.pos_x + kNodeW * 0.5f) * scale, region.y + to.pos_y * scale);
    return true;
}

int ZSlateAnimatorWindow::HitAssetChip(const ZSlate::UIRect& region, const ZSlate::Vector2& point, int& out_slot) const
{
    out_slot = 0;
    if (m_Controller == nullptr)
    {
        return -1;
    }
    const float scale = m_BuiltScale > 0.5f ? m_BuiltScale : 1.0f;

    for (int i = static_cast<int>(m_Controller->states.size()) - 1; i >= 0; --i)
    {
        const AnimatorState& state = m_Controller->states[static_cast<size_t>(i)];
        for (const AssetSlot slot : {AssetSlot::Clip, AssetSlot::SkelMap})
        {
            if (AssetSlotPath(state, slot)[0] == '\0')
            {
                continue;  // nothing to drag out of an empty line
            }
            if (RectContains(AssetChipRect(region, scale, state, slot), point))
            {
                out_slot = static_cast<int>(slot);
                return i;
            }
        }
    }
    return -1;
}

void ZSlateAnimatorWindow::StartAssetDragOut(const ZSlate::Vector2& /*mouse*/)
{
    if (m_Controller == nullptr || m_DragOutState < 0 || m_DragOutState >= static_cast<int>(m_Controller->states.size()))
    {
        return;
    }
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(m_DragOutState)];

    // Same op the Content Browser publishes, built by the same shared helper:
    // stored (content-root-relative) path -> ABSOLUTE payload path.
    std::shared_ptr<FAssetDragDropOp> op =
        EditorDragDrop::MakeAssetDragOperation(AssetSlotPath(state, static_cast<AssetSlot>(m_DragOutSlot)));
    if (op == nullptr)
    {
        return;
    }

    // No SlateInputRouter started this gesture (the canvas is not a widget), so
    // nobody else will clear the channel: EndAssetDragOut() owns that.
    ZSlate::SetActiveDragOperation(op);
    m_DragOutActive = true;
}

void ZSlateAnimatorWindow::EndAssetDragOut()
{
    if (m_DragOutActive && ZSlate::GetActiveDragOperation() != nullptr)
    {
        ZSlate::SetActiveDragOperation(nullptr);
    }
    m_DragOutActive = false;
    m_DragOutArmed = false;
    m_DragOutState = -1;
    m_DragOutSlot = 0;
}

int ZSlateAnimatorWindow::HitTransition(const ZSlate::UIRect& region, const ZSlate::Vector2& point) const
{
    if (m_Controller == nullptr)
    {
        return -1;
    }
    const float scale = m_BuiltScale > 0.5f ? m_BuiltScale : 1.0f;
    const float tolerance = 7.0f * scale;

    for (int i = static_cast<int>(m_Controller->transitions.size()) - 1; i >= 0; --i)
    {
        ZSlate::Vector2 from {0.0f, 0.0f};
        ZSlate::Vector2 to {0.0f, 0.0f};
        if (!GetTransitionEndpoints(region, scale, m_Controller->transitions[static_cast<size_t>(i)], from, to))
        {
            continue;
        }
        if (DistanceToSegment(point, from, to) <= tolerance)
        {
            return i;
        }
    }
    return -1;
}

bool ZSlateAnimatorWindow::HitPort(const ZSlate::UIRect& region, const ZSlate::Vector2& point, int state_index) const
{
    if (m_Controller == nullptr || state_index < 0 || state_index >= static_cast<int>(m_Controller->states.size()))
    {
        return false;
    }
    const float scale = m_BuiltScale > 0.5f ? m_BuiltScale : 1.0f;
    const float local_x = (point.x - region.x) / scale;
    const float local_y = (point.y - region.y) / scale;
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(state_index)];
    const float port_x = state.pos_x + kNodeW * 0.5f - kPortSize * 0.5f;
    const float port_y = state.pos_y + kNodeH - kPortSize * 0.5f;
    return local_x >= port_x && local_x <= port_x + kPortSize && local_y >= port_y && local_y <= port_y + kPortSize;
}

void ZSlateAnimatorWindow::PaintGraph(ZSlate::ISlateRenderer& renderer, const ZSlate::UIRect& region, float scale) const
{
    if (m_Controller == nullptr)
    {
        return;
    }

    const float node_w = kNodeW * scale;
    const float node_h = kNodeH * scale;
    const float font = 13.0f * scale;
    const float small_font = 11.0f * scale;

    auto node_rect = [&](const AnimatorState& state) {
        return ZSlate::UIRect(region.x + state.pos_x * scale, region.y + state.pos_y * scale, node_w, node_h);
    };
    auto anchor_out = [&](const AnimatorState& state) {
        return ZSlate::Vector2(region.x + (state.pos_x + kNodeW * 0.5f) * scale, region.y + (state.pos_y + kNodeH) * scale);
    };

    // ---- Edges -------------------------------------------------------------
    for (int i = 0; i < static_cast<int>(m_Controller->transitions.size()); ++i)
    {
        const AnimatorTransition& transition = m_Controller->transitions[static_cast<size_t>(i)];
        ZSlate::Vector2 from {0.0f, 0.0f};
        ZSlate::Vector2 to {0.0f, 0.0f};
        if (!GetTransitionEndpoints(region, scale, transition, from, to))
        {
            continue;
        }

        const bool selected = m_SelectedTransition == i;
        const bool touched = m_SelectedState == transition.from_state || m_SelectedState == transition.to_state;
        // Unconditioned transitions are the "always take this" ones (the default
        // arrow Unity draws thick): make them read differently at a glance.
        UIColor color;
        if (selected)
        {
            color = UIColor(0.40f, 0.88f, 0.98f, 1.0f);
        }
        else if (transition.conditions.empty())
        {
            color = UIColor(0.55f, 0.78f, 0.55f, 1.0f);
        }
        else
        {
            color = touched ? UIColor(0.95f, 0.80f, 0.40f, 1.0f) : UIColor(0.60f, 0.64f, 0.72f, 1.0f);
        }
        DrawArrow(renderer, from, to, (selected ? 3.5f : (touched ? 3.0f : 2.0f)) * scale, color);

        // A grab dot on the selected arrow: it makes "this is what the strip
        // below is editing" unambiguous.
        if (selected)
        {
            const ZSlate::UIRect dot(from.x + (to.x - from.x) * 0.5f - 3.0f * scale,
                                     from.y + (to.y - from.y) * 0.5f - 3.0f * scale,
                                     6.0f * scale,
                                     6.0f * scale);
            renderer.DrawQuad(dot, UIColor(0.95f, 0.98f, 1.0f, 1.0f));
        }
    }

    // ---- Pending connection ------------------------------------------------
    if (m_Connecting && m_ConnectFrom >= 0 && m_ConnectFrom < static_cast<int>(m_Controller->states.size()))
    {
        const AnimatorState& from = m_Controller->states[static_cast<size_t>(m_ConnectFrom)];
        DrawArrow(renderer, anchor_out(from), m_ConnectCursor, 2.0f * scale, UIColor(0.40f, 0.85f, 0.95f, 1.0f));
    }

    // ---- Nodes -------------------------------------------------------------
    for (size_t i = 0; i < m_Controller->states.size(); ++i)
    {
        const AnimatorState& state = m_Controller->states[i];
        const ZSlate::UIRect rect = node_rect(state);
        const bool is_default = m_Controller->default_state == static_cast<int>(i);
        const bool is_selected = m_SelectedState == static_cast<int>(i);
        // An incoming Content Browser drag paints its target node cyan.
        const bool is_drop_target = m_DropHoverState == static_cast<int>(i);

        UIColor fill = is_selected ? UIColor(0.20f, 0.34f, 0.52f, 1.0f) : UIColor(0.16f, 0.17f, 0.20f, 1.0f);
        renderer.DrawQuad(rect, fill);
        renderer.DrawRect(rect,
                          is_drop_target ? UIColor(0.40f, 0.88f, 0.98f, 1.0f)
                                         : (is_selected ? UIColor(0.55f, 0.80f, 1.00f, 1.0f)
                                                        : (is_default ? UIColor(0.85f, 0.70f, 0.30f, 1.0f)
                                                                      : UIColor(0.34f, 0.36f, 0.42f, 1.0f))),
                          (is_selected || is_drop_target) ? 2.0f * scale : 1.0f);

        // Title: "[*] name" for the default state.
        std::string title = (is_default ? "* " : "") + std::string(state.name.c_str());
        renderer.DrawTextLabel(ZSlate::UIRect(rect.x + 6.0f * scale, rect.y + 4.0f * scale, rect.w - 12.0f * scale, font + 4.0f),
                               title,
                               font,
                               UIColor(0.94f, 0.95f, 0.98f, 1.0f),
                               TextAnchor::MiddleLeft,
                               TextWrapMode::NoWrap);

        // Asset lines: the clip and the anim->skeleton map. Each is a drag-out
        // handle, drawn and hit-tested from the SAME rect (AssetChipRect).
        for (const AssetSlot slot : {AssetSlot::Clip, AssetSlot::SkelMap})
        {
            const char* raw_path = AssetSlotPath(state, slot);
            const bool has_path = raw_path[0] != '\0';
            const std::string placeholder = (slot == AssetSlot::Clip) ? "(no clip)" : "(no map)";
            std::string text = has_path ? std::string(raw_path) : placeholder;
            const size_t slash = text.find_last_of("/\\");
            if (slash != std::string::npos && slash + 1 < text.size())
            {
                text = text.substr(slash + 1);
            }

            const ZSlate::UIRect chip = AssetChipRect(region, scale, state, slot);
            const bool chip_hot = has_path && ((m_HoverChipState == static_cast<int>(i) && m_HoverChipSlot == static_cast<int>(slot)) ||
                                               (m_DragOutActive && m_DragOutState == static_cast<int>(i) &&
                                                m_DragOutSlot == static_cast<int>(slot)));
            if (chip_hot)
            {
                renderer.DrawQuad(chip, UIColor(0.14f, 0.26f, 0.34f, 1.0f));
                renderer.DrawRect(chip, UIColor(0.40f, 0.88f, 0.98f, 1.0f), 1.0f);
            }
            renderer.DrawTextLabel(chip,
                                   text,
                                   small_font,
                                   !has_path ? UIColor(0.75f, 0.45f, 0.45f, 1.0f)
                                             : (chip_hot ? UIColor(0.85f, 0.95f, 1.0f, 1.0f) : kDimColor),
                                   TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap);
        }

        // Port: drag from here onto another node to create a transition.
        const ZSlate::UIRect port(rect.x + rect.w * 0.5f - kPortSize * 0.5f * scale,
                                  rect.y + rect.h - kPortSize * 0.5f * scale,
                                  kPortSize * scale,
                                  kPortSize * scale);
        renderer.DrawQuad(port, UIColor(0.30f, 0.62f, 0.86f, 1.0f));
        renderer.DrawRect(port, UIColor(0.70f, 0.85f, 0.98f, 1.0f), 1.0f);
    }

    if (m_Controller->states.empty())
    {
        renderer.DrawTextLabel(region,
                               "No states -- use \"+ State\" or the States tab.",
                               font,
                               kDimColor,
                               TextAnchor::MiddleCenter,
                               TextWrapMode::NoWrap);
    }

    // No host draws FDragDropOperation::DecoratorText, so the outgoing drag has
    // to draw its own label. It is clipped to the canvas: once the cursor leaves
    // the canvas the label goes with it.
    if (m_DragOutActive && m_DragOutState >= 0 && m_DragOutState < static_cast<int>(m_Controller->states.size()))
    {
        const AnimatorState& state = m_Controller->states[static_cast<size_t>(m_DragOutState)];
        std::string name = std::string(AssetSlotPath(state, static_cast<AssetSlot>(m_DragOutSlot)));
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos && slash + 1 < name.size())
        {
            name = name.substr(slash + 1);
        }
        renderer.DrawTextLabel(ZSlate::UIRect(m_DropCursor.x + 12.0f * scale, m_DropCursor.y + 8.0f * scale, 260.0f * scale, font + 6.0f),
                               "drag " + name + " -> drop on an asset field",
                               font,
                               UIColor(0.95f, 0.98f, 1.0f, 1.0f),
                               TextAnchor::MiddleLeft,
                               TextWrapMode::NoWrap);
    }

    // Drag decorator: follows the cursor so the user sees what will land where.
    if (m_PendingDragOp != nullptr)
    {
        const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(m_PendingDragOp);
        if (asset_op != nullptr)
        {
            std::string name = asset_op->AssetPath;
            const size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos && slash + 1 < name.size())
            {
                name = name.substr(slash + 1);
            }

            const std::string type = NormalizeAssetType(asset_op->AssetPath);
            std::string suffix;
            UIColor color = UIColor(0.95f, 0.98f, 1.0f, 1.0f);
            if (m_DropHoverState < 0)
            {
                suffix = (type == "animatorcontroller") ? " -> open" : " (drop on a state)";
            }
            else if (type == "animationasset")
            {
                suffix = " -> clip";
            }
            else if (type == "animskelmap")
            {
                suffix = " -> anim skel map";
            }
            else
            {
                suffix = " (unsupported here)";
                color = UIColor(0.90f, 0.55f, 0.55f, 1.0f);
            }

            renderer.DrawTextLabel(ZSlate::UIRect(m_DropCursor.x + 12.0f * scale, m_DropCursor.y + 8.0f * scale, 240.0f * scale, font + 6.0f),
                                   name + suffix,
                                   font,
                                   color,
                                   TextAnchor::MiddleLeft,
                                   TextWrapMode::NoWrap);
        }
    }
}

bool ZSlateAnimatorWindow::HandleGraphInput(const ZSlate::UIRect& region, const ZSlate::Vector2& mouse, bool hovered)
{
    if (m_Controller == nullptr)
    {
        return false;
    }

    EditorSlateHost& host = EditorSlateHost::Get();
    const bool inside = hovered && RectContains(region, mouse);

    if (!inside)
    {
        // A drag that leaves the canvas still follows the pointer, so a release
        // anywhere ends it -- including a drag-out that is over another window
        // now (that is the whole point of the gesture).
        if (!host.IsLeftDown() || host.WasLeftReleasedThisFrame())
        {
            m_DraggingNode = false;
            m_Connecting = false;
            EndAssetDragOut();
        }
        return m_DragOutActive;
    }

    const float scale = m_BuiltScale > 0.5f ? m_BuiltScale : 1.0f;
    const float canvas_w = region.w / scale;
    const float canvas_h = region.h / scale;

    // Selecting a node or an arrow rebuilds the chrome (the strip under the
    // canvas appears / disappears), so only a real change may set m_UiDirty:
    // a rebuild drops a text box's keyboard focus.
    auto select = [this](int state_index, int transition_index) {
        if (m_SelectedState == state_index && m_SelectedTransition == transition_index)
        {
            return;
        }
        m_SelectedState = state_index;
        m_SelectedTransition = transition_index;
        m_UiDirty = true;
    };

    // Right-click deletes the arrow under the cursor. The host has no
    // "right pressed this frame", so the edge is latched from IsRightDown().
    const bool right_down = host.IsRightDown();
    const bool right_pressed = right_down && !m_RightWasDown;
    m_RightWasDown = right_down;
    if (right_pressed)
    {
        const int arrow = HitTransition(region, mouse);
        if (arrow >= 0)
        {
            RemoveTransition(arrow);
        }
        return true;  // the canvas swallows right-clicks either way
    }

    if (host.WasLeftPressedThisFrame())
    {
        // Ports win over the body: they are small and sit on top of the node.
        for (int i = 0; i < static_cast<int>(m_Controller->states.size()); ++i)
        {
            if (HitPort(region, mouse, i))
            {
                m_Connecting = true;
                m_ConnectFrom = i;
                m_ConnectCursor = mouse;
                return true;
            }
        }

        // An asset line wins over the node body: it is the drag-out handle, and
        // it is the only way to carry that path to another window.
        int chip_slot = 0;
        const int chip = HitAssetChip(region, mouse, chip_slot);
        if (chip >= 0)
        {
            m_DragOutArmed = true;
            m_DragOutState = chip;
            m_DragOutSlot = chip_slot;
            m_DragOutOrigin = mouse;
            select(chip, -1);
            return true;
        }

        const int hit = HitNode(region, mouse);
        if (hit >= 0)
        {
            m_DraggingNode = true;
            m_DragNode = hit;
            select(hit, -1);
            const AnimatorState& state = m_Controller->states[static_cast<size_t>(hit)];
            m_DragGrabOffset = ZSlate::Vector2(mouse.x - (region.x + state.pos_x * scale),
                                               mouse.y - (region.y + state.pos_y * scale));
            return true;
        }

        // Nodes first, arrows second: an arrow that runs under a node must not
        // steal the node's clicks.
        const int arrow = HitTransition(region, mouse);
        if (arrow >= 0)
        {
            select(-1, arrow);
            return true;
        }

        select(-1, -1);
        return true;
    }

    if (host.IsLeftDown())
    {
        // Arming a drag-out is not the gesture yet: a few pixels of travel first,
        // so clicking the clip line still just selects the node.
        if (m_DragOutArmed && !m_DragOutActive)
        {
            const ZSlate::Vector2 delta(mouse.x - m_DragOutOrigin.x, mouse.y - m_DragOutOrigin.y);
            if (std::sqrt(delta.x * delta.x + delta.y * delta.y) > 5.0f * scale)
            {
                StartAssetDragOut(mouse);
            }
        }
        if (m_DragOutActive)
        {
            m_DropCursor = mouse;  // the decorator follows the cursor
            return true;           // no node drag / connect while carrying an asset out
        }

        if (m_DraggingNode && m_DragNode >= 0 && m_DragNode < static_cast<int>(m_Controller->states.size()))
        {
            AnimatorState& state = m_Controller->states[static_cast<size_t>(m_DragNode)];
            // Deliberately no m_UiDirty: repainting happens every frame anyway,
            // and a rebuild would drop a text box's keyboard focus.
            state.pos_x = std::clamp((mouse.x - m_DragGrabOffset.x - region.x) / scale, 0.0f, std::max(0.0f, canvas_w - kNodeW));
            state.pos_y = std::clamp((mouse.y - m_DragGrabOffset.y - region.y) / scale, 0.0f, std::max(0.0f, canvas_h - kNodeH));
            m_ModelDirty = true;
        }
        if (m_Connecting)
        {
            m_ConnectCursor = mouse;
        }
        return true;
    }

    if (host.WasLeftReleasedThisFrame())
    {
        // The drop itself is delivered by the target window's router; our job is
        // to unpublish the op so the channel does not stay hot.
        if (m_DragOutActive)
        {
            EndAssetDragOut();
            return true;
        }
        if (m_DragOutArmed)
        {
            m_DragOutArmed = false;
            m_DragOutState = -1;
            return true;
        }

        if (m_Connecting && m_ConnectFrom >= 0)
        {
            const int target = HitNode(region, mouse);
            if (target >= 0 && target != m_ConnectFrom)
            {
                // Reuse the model op path so duplicates and state-count checks
                // stay in one place.
                bool exists = false;
                for (const AnimatorTransition& transition : m_Controller->transitions)
                {
                    if (transition.from_state == m_ConnectFrom && transition.to_state == target)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                {
                    AnimatorTransition transition;
                    transition.from_state = m_ConnectFrom;
                    transition.to_state = target;
                    transition.duration = 0.25f;
                    m_Controller->transitions.push_back(transition);
                    // Select what was just drawn, so the strip opens on it and
                    // its conditions / fade can be set immediately.
                    m_SelectedState = -1;
                    m_SelectedTransition = static_cast<int>(m_Controller->transitions.size()) - 1;
                    m_UiDirty = true;
                    m_ModelDirty = true;
                }
            }
        }
        m_Connecting = false;
        m_ConnectFrom = -1;
        m_DraggingNode = false;
        m_DragNode = -1;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
void ZSlateAnimatorWindow::BuildChrome(float scale)
{
    const float font = 14.0f * scale;

    auto root = std::make_shared<SVerticalBox>();

    // ---- Toolbar ------------------------------------------------------------
    auto toolbar = std::make_shared<SHorizontalBox>();
    auto pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
        return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 5.0f, 0.0f));
    };

    pad(toolbar->AddSlot(MakeButton("New", font, [this]() {
        std::string error;
        const std::string path = m_PathBox != nullptr ? m_PathBox->Text : std::string();
        if (!NewController(path.empty() ? "NewAnimatorController.controller.zasset" : path, error))
        {
            LOG_ERROR(ZEditor, "Animator: new failed: {}", error);
        }
    })));
    pad(toolbar->AddSlot(MakeButton("Open", font, [this]() {
        std::string error;
        const std::string path = m_PathBox != nullptr ? m_PathBox->Text : std::string();
        if (!OpenController(path, error))
        {
            LOG_ERROR(ZEditor, "Animator: open failed: {}", error);
        }
    })));
    pad(toolbar->AddSlot(MakeButton("Save", font, [this]() {
        std::string error;
        if (!SaveController(error))
        {
            LOG_ERROR(ZEditor, "Animator: save failed: {}", error);
        }
    })));

    m_PathBox = MakeTextBox(m_AssetPath, font, 320.0f * scale, "Assets/<name>.controller.zasset",
                            [this](const std::string& t) { m_AssetPath = t; });
    pad(toolbar->AddSlot(m_PathBox));

    m_StatusText = MakeLabel("", font);
    m_StatusText->Color = kDimColor;
    pad(toolbar->AddSlot(m_StatusText));

    root->AddSlot(toolbar).AutoSize().SetPadding(ZSlate::FMargin(5.0f, 4.0f, 4.0f, 6.0f));

    // ---- Body ---------------------------------------------------------------
    auto body = std::make_shared<SHorizontalBox>();

    auto make_panel = [&](const std::string& title, const std::shared_ptr<ZSlate::SWidget>& in_content) {
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = kPanelColor;
        border->Padding = ZSlate::FMargin(6.0f, 6.0f, 6.0f, 6.0f);
        auto column = std::make_shared<SVerticalBox>();
        column->AddSlot(MakeLabel(title, font)).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 6.0f));
        column->AddSlot(in_content).Fill(1.0f);
        border->SetContent(column);
        return border;
    };

    if (m_Controller == nullptr)
    {
        auto empty = std::make_shared<SScrollBox>();
        empty->AddChild(MakeLabel("No controller -- click New or Open above.", font));
        body->AddSlot(make_panel("Animator", empty)).Fill(1.0f);
        root->AddSlot(body).Fill(1.0f);
        m_Chrome = root;
        m_GraphSlot.reset();
        return;
    }

    // Parameters stay visible on every tab: a transition is meaningless without
    // the parameters it tests.
    auto parameters = std::make_shared<SScrollBox>();
    parameters->AddChild(MakeButton("+ Parameter", font, [this]() { AddParameter(); }));
    parameters->AddChild(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f, 4.0f * scale)));
    BuildParameterRows(parameters, scale);
    body->AddSlot(make_panel("Parameters", parameters)).Fill(1.0f).SetPadding(ZSlate::FMargin(2.0f, 0.0f, 3.0f, 0.0f));

    // ---- Right pane: tab row + one content slot ----------------------------
    auto right = std::make_shared<SVerticalBox>();

    auto tabs = std::make_shared<SHorizontalBox>();
    auto tab_button = [&](const char* label, ViewTab tab) {
        auto button = MakeButton(label, font, [this, tab]() {
            m_Tab = tab;
            m_UiDirty = true;
        });
        if (m_Tab == tab)
        {
            button->NormalColor = kAccent;
            button->HoverColor = kAccentHover;
        }
        return button;
    };
    pad(tabs->AddSlot(tab_button("Graph", ViewTab::Graph)));
    pad(tabs->AddSlot(tab_button("States", ViewTab::States)));
    pad(tabs->AddSlot(tab_button("Transitions", ViewTab::Transitions)));
    if (m_Tab == ViewTab::Graph)
    {
        pad(tabs->AddSlot(MakeButton("Auto Layout", font, [this]() { AutoLayout(); })));
        pad(tabs->AddSlot(MakeButton("+ State", font, [this]() { AddState(); })));
    }
    right->AddSlot(tabs).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    std::shared_ptr<ZSlate::SWidget> content;
    if (m_PickField != PickField::None)
    {
        // The picker takes over the whole pane while it is open.
        m_GraphSlot.reset();
        auto picker = std::make_shared<SVerticalBox>();
        BuildPickerPanel(picker, scale);
        content = picker;
    }
    else if (m_Tab == ViewTab::Graph)
    {
        // An empty SBox: it contributes no widgets, but after ArrangeChildren it
        // caches the geometry of the right pane, which is exactly the rect the
        // hand-painted graph must cover. Reading it back keeps the canvas and
        // the widget layout from drifting apart.
        m_GraphSlot = std::make_shared<SBox>();
        content = m_GraphSlot;
    }
    else
    {
        m_GraphSlot.reset();
        auto scroll = std::make_shared<SScrollBox>();
        if (m_Tab == ViewTab::States)
        {
            scroll->AddChild(MakeButton("+ State", font, [this]() { AddState(); }));
            scroll->AddChild(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f, 4.0f * scale)));
            BuildStateRows(scroll, scale);
        }
        else
        {
            scroll->AddChild(MakeButton("+ Transition", font, [this]() { AddTransition(); }));
            scroll->AddChild(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f, 4.0f * scale)));
            BuildTransitionRows(scroll, scale);
        }
        content = scroll;
    }
    right->AddSlot(content).Fill(1.0f);
    if (m_Tab == ViewTab::Graph && m_PickField == PickField::None)
    {
        // Editing strip for whatever is selected on the canvas (each no-ops when
        // its own selection is empty, and the two are mutually exclusive, so at
        // most one strip exists). Absent when nothing is selected, so the canvas
        // keeps the whole pane.
        BuildSelectedStateEditor(right, scale);
        BuildSelectedTransitionEditor(right, scale);
    }

    body->AddSlot(right).Fill(2.2f).SetPadding(ZSlate::FMargin(3.0f, 0.0f, 2.0f, 0.0f));

    root->AddSlot(body).Fill(1.0f);

    m_Chrome = root;
}

void ZSlateAnimatorWindow::UpdateStatus()
{
    if (m_StatusText == nullptr)
    {
        return;
    }
    if (m_Controller == nullptr)
    {
        m_StatusText->Text = "(no controller)";
        return;
    }
    char buffer[160];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s  |  %zu param(s), %zu state(s), %zu transition(s)%s",
                  m_AssetPath.empty() ? "(unsaved)" : m_AssetPath.c_str(),
                  m_Controller->parameters.size(),
                  m_Controller->states.size(),
                  m_Controller->transitions.size(),
                  m_ModelDirty ? "  *" : "");
    m_StatusText->Text = buffer;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
std::string ZSlateAnimatorWindow::ResolvePath(const std::string& in_path) const
{
    if (in_path.empty())
    {
        return std::string();
    }
    std::filesystem::path path(in_path);
    if (path.is_absolute())
    {
        return path.generic_string();
    }
    auto* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return path.generic_string();
    }
    return (project_info->GetProjectContent() / path).generic_string();
}

bool ZSlateAnimatorWindow::NewController(const std::string& asset_path, std::string& out_error)
{
    auto* object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        out_error = "ObjectManager unavailable";
        return false;
    }
    Object* produced = object_manager->Produce(TypeOf<AnimatorController>(), 0);
    if (produced == nullptr)
    {
        out_error = "cannot produce an AnimatorController";
        return false;
    }
    auto* controller = static_cast<AnimatorController*>(produced);
    controller->default_state = 0;

    m_Controller = controller;
    m_AssetPath = ResolvePath(asset_path);
    m_SelectedState = -1;
    m_SelectedTransition = -1;
    m_DraggingNode = false;
    m_Connecting = false;
    ClosePicker();
    m_Tab = ViewTab::Graph;
    m_UiDirty = true;
    m_ModelDirty = true;
    return true;
}

bool ZSlateAnimatorWindow::OpenController(const std::string& asset_path, std::string& out_error)
{
    const std::string resolved = ResolvePath(asset_path);
    if (resolved.empty())
    {
        out_error = "empty path";
        return false;
    }
    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        out_error = "AssetManager unavailable";
        return false;
    }
    AnimatorController* loaded = asset_manager->loadAsset<AnimatorController>(resolved.c_str());
    if (loaded == nullptr)
    {
        out_error = "cannot load '" + resolved + "'";
        return false;
    }
    m_Controller = loaded;
    m_AssetPath = resolved;
    m_SelectedState = -1;
    m_SelectedTransition = -1;
    m_DraggingNode = false;
    m_Connecting = false;
    m_PickField = PickField::None;
    m_PickStateIndex = -1;
    m_PickRows.clear();
    // pos_x / pos_y did not exist before the graph view, so every state of a
    // controller authored without it sits at (0,0) and would stack in the
    // corner. Arrange it once on load; an authored layout is left alone.
    bool has_layout = false;
    for (const AnimatorState& state : m_Controller->states)
    {
        if (state.pos_x != 0.0f || state.pos_y != 0.0f)
        {
            has_layout = true;
            break;
        }
    }
    if (!has_layout && !m_Controller->states.empty())
    {
        AutoLayout();
    }
    m_UiDirty = true;
    // AutoLayout counts as an authored change only if it actually moved
    // something onto the canvas.
    m_ModelDirty = false;
    return true;
}

bool ZSlateAnimatorWindow::SaveController(std::string& out_error)
{
    if (m_Controller == nullptr)
    {
        out_error = "no controller to save";
        return false;
    }
    if (m_AssetPath.empty())
    {
        out_error = "no asset path";
        return false;
    }
    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        out_error = "AssetManager unavailable";
        return false;
    }
    if (!asset_manager->WriteObjectToDiskThreadSafe(std::filesystem::path(m_AssetPath), *m_Controller))
    {
        out_error = "write failed for '" + m_AssetPath + "'";
        return false;
    }
    m_ModelDirty = false;
    LOG_INFO(ZEditor, "Animator: saved '{}'", m_AssetPath);
    return true;
}

bool ZSlateAnimatorWindow::ShowAndOpen(const std::string& asset_path, std::string& out_error)
{
    if (!OpenController(asset_path, out_error))
    {
        return false;
    }
    m_Open = true;
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame entry point
// ---------------------------------------------------------------------------
void ZSlateAnimatorWindow::OnGUI()
{
    float ui_scale = EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
    {
        ui_scale = 1.0f;
    }

    if (m_Chrome == nullptr || m_UiDirty || ui_scale != m_BuiltScale)
    {
        m_UiDirty = false;
        BuildChrome(ui_scale);
        m_BuiltScale = ui_scale;
        m_Input.Reset();
    }

    UpdateStatus();
    UpdatePickFilter();

    const float* native_rect = NativeRect();
    float pos_x = native_rect[0];
    float pos_y = native_rect[1];
    float avail_w = native_rect[2];
    float avail_h = native_rect[3];
    if (avail_w < 1.0f)
    {
        avail_w = 1.0f;
    }
    if (avail_h < 1.0f)
    {
        avail_h = 1.0f;
    }

    const ZSlate::UIRect panel_region(pos_x, pos_y, avail_w, avail_h);
    const FGeometry panel_geom(ZSlate::Vector2(pos_x, pos_y), ZSlate::Vector2(avail_w, avail_h));

    auto& overlay = ZSlateEditorOverlay::Get();

    auto paint_all = [&](FPaintContext& ctx) {
        if (m_Chrome != nullptr)
        {
            m_Chrome->CacheDesiredSize();
            m_Chrome->Paint(ctx, panel_geom);
        }
    };

    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlateEditorOverlay::kZPanel);
        FPaintContext ctx;
        ctx.Renderer = &overlay;
        ctx.LayerId = 0;
        renderer.PushClipRect(panel_region, true);
        paint_all(ctx);

        // The graph is hand-painted into the rect the placeholder SBox was
        // arranged into, so it can never disagree with the widget layout.
        if (m_Tab == ViewTab::Graph && m_GraphSlot != nullptr && m_Controller != nullptr)
        {
            m_GraphRect = m_GraphSlot->GetCachedGeometry().ToRect();
            if (m_GraphRect.w > 1.0f && m_GraphRect.h > 1.0f)
            {
                renderer.PushClipRect(m_GraphRect, true);
                PaintGraph(overlay, m_GraphRect, ui_scale);
                renderer.PopClipRect();
            }
        }
        renderer.PopClipRect();
    }

    // ---- Input --------------------------------------------------------------
    EditorSlateHost& host = EditorSlateHost::Get();
    const int surface_id = EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_panel = host.IsSurfaceHovered(surface_id, mouse);

    // ---- Cross-window drag-drop (Content Browser asset -> canvas) -----------
    // Cache the in-flight op while it is published: on the release frame the
    // source router may already have cleared the channel (window draw order),
    // so act on this copy -- same reason as ZSlateSceneWindow.
    if (auto active = ZSlate::GetActiveDragOperation())
    {
        m_PendingDragOp = active;
    }
    // The release frame counts as part of the gesture: `IsLeftDown()` is already
    // false by then, so keying off it alone would never see the drop.
    const bool drag_incoming =
        (m_PendingDragOp != nullptr) && (host.IsLeftDown() || host.WasLeftReleasedThisFrame());

    // The canvas owns its own gestures (node drag, drag-to-connect), so it gets
    // first refusal on the pointer and the chrome is told the click never
    // happened while a gesture is running.
    const bool graph_enabled = (m_Controller != nullptr) && (m_Tab == ViewTab::Graph) && (m_GraphSlot != nullptr);
    bool graph_consumed = false;

    if (drag_incoming && graph_enabled)
    {
        // A node under an incoming drag is a DROP target, not a drag handle: our
        // own gestures stay off so a drop cannot also move the node.
        m_DropCursor = mouse;
        m_DropHoverState = RectContains(m_GraphRect, mouse) ? HitNode(m_GraphRect, mouse) : -1;
        if (host.WasLeftReleasedThisFrame() && RectContains(m_GraphRect, mouse))
        {
            ApplyAssetDrop(mouse);
        }
        graph_consumed = true;
    }
    else if (graph_enabled)
    {
        graph_consumed = HandleGraphInput(m_GraphRect, mouse, over_panel);
        m_DropHoverState = -1;
        // Computed here (one frame before it is painted) rather than in
        // PaintGraph, which must stay const.
        if (over_panel && !m_DragOutActive)
        {
            m_HoverChipState = HitAssetChip(m_GraphRect, mouse, m_HoverChipSlot);
        }
        else
        {
            m_HoverChipState = -1;
        }
    }

    // Gesture over (button up): drop the cache so a stale op cannot fire later.
    if (!host.IsLeftDown())
    {
        m_PendingDragOp.reset();
        m_DropHoverState = -1;
    }

    const bool over_chrome = over_panel && !(graph_enabled && RectContains(m_GraphRect, mouse));

    m_Input.ProcessMouse(m_Chrome,
                         mouse,
                         over_chrome,
                         host.IsLeftDown() && !graph_consumed,
                         over_chrome ? host.GetWheelDelta() : 0.0f);

    if (m_Input.HasKeyboardFocus())
    {
        host.NotifyNativeTextInputActive();
        for (unsigned int codepoint : host.GetCharsThisFrame())
        {
            m_Input.ProcessChar(codepoint);
        }
        for (EKey key : host.GetKeysThisFrame())
        {
            if (key == EKey::Backspace || key == EKey::Enter || key == EKey::Delete)
            {
                m_Input.ProcessKey(key);
            }
        }
    }
}
