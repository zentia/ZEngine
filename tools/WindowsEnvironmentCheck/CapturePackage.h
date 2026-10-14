#pragma once
#include "Collection.h"
#include "EvidenceFiles.h"

namespace zengine::envcheck::collector
{
void CollectDumpEnvironment(const Options&, const DumpMetadata&, Result&, const std::atomic_bool&,
    const std::function<void(const std::string&, bool)>&);
void PackageDumpEvidence(Result&, std::vector<std::string>, const std::atomic_bool&,
    const std::function<void(const std::string&, bool)>&);
}
