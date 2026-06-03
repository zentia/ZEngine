#include "Editor/Prefab/PropertyModification.h"

#include "Runtime/Core/JsonSerialize/JSONRead.h"
#include "Runtime/Core/JsonSerialize/JSONWrite.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryWrite.h"
#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"

// PropertyModification is a plain Serializable struct (DECLARE_SERIALIZE) — not an
// Object subclass — so it doesn't go through IMPLEMENT_OBJECT_SERIALIZE. We just need
// the explicit template instantiations so the four StreamedBinary/JSON transfer
// functions are emitted in this TU. These mirror what
// INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL produces for Object subclasses.
template void PropertyModification::Transfer<JSONRead>(JSONRead&);
template void PropertyModification::Transfer<JSONWrite>(JSONWrite&);
template void PropertyModification::Transfer<YAMLRead>(YAMLRead&);
template void PropertyModification::Transfer<YAMLWrite>(YAMLWrite&);
template void PropertyModification::Transfer<StreamedBinaryRead>(StreamedBinaryRead&);
template void PropertyModification::Transfer<StreamedBinaryWrite>(StreamedBinaryWrite&);
template void PropertyModification::Transfer<SafeBinaryRead>(SafeBinaryRead&);
template void PropertyModification::Transfer<GenerateTypeTreeTransfer>(GenerateTypeTreeTransfer&);
