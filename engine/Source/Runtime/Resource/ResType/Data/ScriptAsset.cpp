#include "ScriptAsset.h"

// ZEngine RTTI / serialisation boilerplate. The auto-registrar object inside
// IMPLEMENT_REGISTER_CLASS runs at static-init time and registers ScriptAsset
// with TypeManager - no manual entry in RegisterRuntime.cpp is required.
//
// IMPLEMENT_OBJECT_SERAILIZE (note the engine-internal typo - kept for
// consistency with every other res_type/data file) wires up
// VirtualRedirectTransfer for the JSON / binary transfer paths.
//
// INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED forces the Transfer<> template body
// to be emitted in this TU with the EXPORTDLL linkage decoration so that
// ZEditor (which links against ZRuntime.dll) sees the symbol.
IMPLEMENT_REGISTER_CLASS(ScriptAsset);
IMPLEMENT_OBJECT_SERAILIZE(ScriptAsset);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(ScriptAsset)

// Phase 2: ScriptAsset is not yet referenced by serialised scenes / prefabs,
// so Transfer() is intentionally empty. Phase 5 will add the field bindings
// when TypeScriptComponent starts persisting `PPtr<ScriptAsset> m_Script` and
// per-instance field overrides.
template<typename TransferFunction>
void ScriptAsset::Transfer(TransferFunction& transfer)
{
}
