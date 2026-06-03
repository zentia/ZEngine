#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Base/EngineSystem.h"

#include <filesystem>
class Object;

class ResourceManager : public IEngineSystem
{
public:
    Object* Load(std::filesystem::path& path);
    using container = std::unordered_multimap<std::string, PPtr<Object>>;
    using iterator = container::iterator;
    using range = std::pair<iterator, iterator>;
    range GetPathRange(std::filesystem::path& path);
    container m_Container;

protected:
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    bool Initialize() override { return true; }
    void Shutdown() override {}
};