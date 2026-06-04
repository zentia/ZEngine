#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Command/CommandSystem.h"

#if defined(_WIN32)

class RenderDocLoader : public IEngineSystem
{
public:
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {GET_SYSTEM_TYPE(CommandSystem)}; }

protected:
    bool Initialize() override;
    void Shutdown() override {}
};

#endif
