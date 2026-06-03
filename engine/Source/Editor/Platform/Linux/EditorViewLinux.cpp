#include "Editor/Platform/Interface/EditorView.h"

#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"

// Mirror of Platform/Windows/EditorView.cpp -- native dock hosting is platform-agnostic
// (no ImGui host window). See that file for the full rationale.

EditorView::EditorView(EditorUI* editor_ui, const char* title)
    : m_EditorUi(editor_ui), m_Title(title) {}

EditorWindowState EditorView::BeginGUI()
{
    m_NativeHostActive = false;

    if (!m_Open)
        return EditorWindowState::Closed;

    if (m_EditorUi != nullptr)
    {
        if (DefaultLayout* layout = m_EditorUi->getLayoutManager())
        {
            float rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool is_active = false;
            if (layout->QueryNativeDockPanel(m_Title, rect, is_active))
            {
                if (!is_active)
                    return EditorWindowState::Closed;

                m_NativeRect[0] = rect[0];
                m_NativeRect[1] = rect[1];
                m_NativeRect[2] = rect[2];
                m_NativeRect[3] = rect[3];
                m_NativeHostActive = true;
                return EditorWindowState::Opened;
            }
        }
    }

    return EditorWindowState::Closed;
}

void EditorView::EndGUI()
{
    m_NativeHostActive = false;
}
