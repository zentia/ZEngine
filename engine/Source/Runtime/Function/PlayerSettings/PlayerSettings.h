#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Project/ProjectInfo.h"

class PlayerSettings : public IEngineSystem
{
public:
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {GET_SYSTEM_TYPE(ProjectInfo)}; }
    bool Initialize() override;
    void Shutdown() override {}
    std::string m_ProjectName;
    std::string m_CompanyName;
    std::string m_ProductName;
    std::string m_Version;
};