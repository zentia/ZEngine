#pragma once

#include "runtime/core/math/vector2.h"

#include <memory>

namespace Z
{
    class EditorUI;
    class ZEngine;

    class ZEditor 
    {
        friend class EditorUI;

    public:
        ZEditor();
        virtual ~ZEditor();

        void initialize(ZEngine* engine_runtime);
        void clear();

        void run();

    protected:
        std::shared_ptr<EditorUI> m_editor_ui;
        ZEngine* m_engine_runtime{ nullptr };
    };
} // namespace Zentia
