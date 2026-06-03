#pragma once

#include "Editor/EditorUI/EditorUI.h"
#include "Runtime/BaseClasses/Object.h"

#include <cstdint>

enum class EditorWindowState
{
    Closed,
    Closing,
    Opened,
};

// Engine-owned window-decoration flags for editor panels. They are deliberately
// backend-agnostic. Now that hosting is fully native (the native DockHost draws
// all panel chrome and BeginGUI issues no host window) these are currently
// vestigial -- panel ctors still set them, but BeginGUI no longer consumes them.
// Kept as a stable bitmask for any future native chrome that wants per-panel hints.
enum EditorViewFlags_ : uint32_t
{
    EditorViewFlags_None                  = 0u,
    EditorViewFlags_NoBackground          = 1u << 0,
    EditorViewFlags_MenuBar               = 1u << 1,
    EditorViewFlags_NoTitleBar            = 1u << 2,
    EditorViewFlags_NoResize              = 1u << 3,
    EditorViewFlags_NoMove                = 1u << 4,
    EditorViewFlags_NoCollapse            = 1u << 5,
    EditorViewFlags_NoDocking             = 1u << 6,
    EditorViewFlags_NoBringToFrontOnFocus = 1u << 7,
};
using EditorViewFlags = uint32_t;

class EditorView : public Object
{
public:
    explicit EditorView(EditorUI* editor_ui, const char* title);

    virtual ~EditorView() = default;
    virtual void OnGUI() = 0;
    virtual EditorWindowState BeginGUI();
    void EndGUI();

    // Vestigial since hosting went fully native: every panel paints itself through
    // the BatchedUIRenderer overlay from its dock-leaf rect and reads input from
    // EditorSlateHost. BeginGUI no longer branches on this (there is no ImGui host
    // fallback left), but the virtual is kept so panels can still advertise intent.
    virtual bool SupportsNativeHosting() const { return false; }

    // True between BeginGUI/EndGUI of the current frame when the panel was placed by
    // the native dock tree (its NativeRect() leaf rect is valid this frame).
    bool IsNativeHostActive() const { return m_NativeHostActive; }
    // Dock leaf content rect [x, y, w, h] valid while IsNativeHostActive().
    const float* NativeRect() const { return m_NativeRect; }

    bool m_Open {true};
    const char* m_Title;
    EditorViewFlags m_WindowFlags {EditorViewFlags_None};

protected:
    EditorUI* m_EditorUi {nullptr};
    bool m_NativeHostActive {false};
    float m_NativeRect[4] {0.0f, 0.0f, 0.0f, 0.0f};
};
