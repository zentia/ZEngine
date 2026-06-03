#include "Editor/Platform/Interface/EditorView.h"

#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"

EditorView::EditorView(EditorUI* editor_ui, const char* title)
    : m_EditorUi(editor_ui), m_Title(title) {}

EditorWindowState EditorView::BeginGUI()
{
    m_NativeHostActive = false;

    if (!m_Open)
        return EditorWindowState::Closed;

    // Native dock hosting is the only path. Every editor panel paints itself through the
    // BatchedUIRenderer overlay from its dock-leaf content rect and reads input from
    // EditorSlateHost -- there is no ImGui host window. The dock tree owns placement, so we
    // position the panel by handing it the leaf rect via NativeRect() and only paint the
    // active tab of each leaf (the native tab strip drawn by the host shows the rest).
    //
    // ReconcileNativeTreeWithOpenWindows docks every open panel into the native DockTree each
    // frame before panels paint, so a panel that is open but not yet placed is at most a
    // transient one-frame state -- skip painting it (return Closed) rather than hosting it in
    // a separate ImGui window.
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
    // Native hosting issues no host window, so there is nothing to close here.
    m_NativeHostActive = false;
}
