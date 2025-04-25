#pragma once

#include "runtime/core/math/vector2.h"

#include <memory>

namespace Zentia
{
    class EditorUI;
    class ZentiaEngine;

    class ZentiaEditor 
    {
        friend class EditorUI;

    public:
        ZentiaEditor();
        virtual ~ZentiaEditor();

        void initialize(ZentiaEngine* engine_runtime);
        void clear();

        void run();

    protected:
        std::shared_ptr<EditorUI> m_editor_ui;
        ZentiaEngine* m_engine_runtime{ nullptr };
    };
} // namespace Zentia
