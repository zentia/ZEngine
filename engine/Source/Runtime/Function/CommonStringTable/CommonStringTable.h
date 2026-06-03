#pragma once
#include "../../Core/Base/EngineSystem.h"

class CommonStringTable : public IEngineSystem
{
public:
    const char* FindCommonString(const char* str, size_t length) const;

protected:
    virtual SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    virtual bool Initialize() override { return true; }
    virtual void Shutdown() override {}
};