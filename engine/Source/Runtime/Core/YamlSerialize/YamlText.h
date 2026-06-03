#pragma once

#include "Runtime/Core/JsonSerialize/JSONAllocator.h"

#include <EASTL/string.h>

#include "rapidjson/document.h"

// Block-YAML text codec over the same rapidjson DOM the JSON backend uses.
//
// YAMLWrite / YAMLRead build / consume the identical rapidjson node tree as
// JSONWrite / JSONRead -- the ONLY difference between the two text backends is
// the on-disk encoding. These two free functions are that encoding:
//
//   * EmitYaml  : rapidjson Value tree  -> block-style YAML text (one document)
//   * ParseYaml : block-style YAML text -> rapidjson Value tree
//
// The grammar handled is the constrained subset that EmitYaml produces (block
// mappings, block sequences, and scalars: null / bool / int / double / string),
// plus the common hand-edit shapes (full-line `#` comments, a leading `---`,
// `{}` / `[]` for empty collections). It is deliberately NOT a full YAML 1.x
// parser -- ZEngine only needs faithful round-tripping of Transfer() output, the
// same scope the JSON backend covers. Indentation is spaces only (EmitYaml emits
// 2-space steps); tab-indented input is out of scope.
namespace ZYaml
{
    using YamlValue = rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator>;
    using YamlDocument = rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator>;

    // Serialize `root` into `out` as a single block-YAML document (trailing '\n').
    void EmitYaml(const YamlValue& root, eastl::string& out);

    // Parse `text` into `outDoc`. Returns true on success; on a structural failure
    // the document is left as an empty object so downstream reads degrade to
    // "all fields default" rather than crashing.
    bool ParseYaml(const char* text, YamlDocument& outDoc);
}  // namespace ZYaml
