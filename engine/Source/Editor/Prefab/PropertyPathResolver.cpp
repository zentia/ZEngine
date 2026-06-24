#include "Editor/Prefab/PropertyPathResolver.h"

#include "Runtime/Core/Serialize/SerializationMetaFlags.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace prefab_editor
{

    namespace
    {

        /// Tiny helper: parse a non-negative decimal integer from `str[begin..end)`. Returns
        /// false on empty range, leading/trailing non-digits, or overflow.
        bool ParseUInt(const char* str, size_t begin, size_t end, int32_t& out)
        {
            if (begin >= end)
            {
                return false;
            }
            int64_t acc = 0;
            for (size_t i = begin; i < end; ++i)
            {
                const char c = str[i];
                if (c < '0' || c > '9')
                {
                    return false;
                }
                acc = acc * 10 + (c - '0');
                if (acc > 0x7FFFFFFF)
                {
                    return false;
                }
            }
            out = static_cast<int32_t>(acc);
            return true;
        }

        /// Returns true iff `name` (NUL-terminated) equals `expected` (eastl::string).
        bool TypeTreeNameEquals(const char* name, const eastl::string& expected)
        {
            if (name == nullptr)
            {
                return expected.empty();
            }
            return std::strcmp(name, expected.c_str()) == 0;
        }

    }  // namespace

    // =====================================================================================
    // TokenizePropertyPath
    // -------------------------------------------------------------------------------------
    // State machine over the path string. Three "states":
    //   - `kField`: accumulating a field name; ends on '.' or '['.
    //   - `kBracket`: accumulating digits inside [...]; ends on ']'.
    //   - `kBetween`: just emitted a token, expecting either '.' (start new field), or
    //                 '[' (start new index), or end-of-string.
    // =====================================================================================
    bool TokenizePropertyPath(const eastl::string& path, std::vector<PathToken>& out_tokens)
    {
        out_tokens.clear();
        if (path.empty())
        {
            return false;
        }

        enum class State
        {
            kField,
            kBracket,
            kBetween,
        };

        State state = State::kField;
        size_t segment_begin = 0;
        const char* data = path.c_str();
        const size_t length = path.size();

        for (size_t i = 0; i < length; ++i)
        {
            const char c = data[i];

            switch (state)
            {
                case State::kField:
                {
                    if (c == '.' || c == '[')
                    {
                        if (i == segment_begin)
                        {
                            // Empty field name (e.g. "..foo" or "[" right after start).
                            return false;
                        }
                        PathToken tok;
                        tok.kind = PathToken::Kind::Field;
                        tok.field.assign(data + segment_begin, data + i);
                        out_tokens.push_back(std::move(tok));

                        if (c == '.')
                        {
                            state = State::kField;
                            segment_begin = i + 1;
                        }
                        else  // '['
                        {
                            state = State::kBracket;
                            segment_begin = i + 1;
                        }
                    }
                    // Otherwise keep accumulating the field name.
                    break;
                }
                case State::kBracket:
                {
                    if (c == ']')
                    {
                        int32_t idx = 0;
                        if (!ParseUInt(data, segment_begin, i, idx))
                        {
                            return false;
                        }
                        PathToken tok;
                        tok.kind = PathToken::Kind::Index;
                        tok.array_index = idx;
                        out_tokens.push_back(std::move(tok));

                        state = State::kBetween;
                    }
                    else if (c < '0' || c > '9')
                    {
                        return false;
                    }
                    break;
                }
                case State::kBetween:
                {
                    if (c == '.')
                    {
                        state = State::kField;
                        segment_begin = i + 1;
                    }
                    else if (c == '[')
                    {
                        state = State::kBracket;
                        segment_begin = i + 1;
                    }
                    else
                    {
                        return false;  // unexpected char after a closed bracket
                    }
                    break;
                }
            }
        }

        // Finalise: only the kField state may have a pending segment to Flush.
        switch (state)
        {
            case State::kField:
            {
                if (segment_begin >= length)
                {
                    // Trailing '.' with no field after it.
                    return false;
                }
                PathToken tok;
                tok.kind = PathToken::Kind::Field;
                tok.field.assign(data + segment_begin, data + length);
                out_tokens.push_back(std::move(tok));
                break;
            }
            case State::kBracket:
            {
                // Unterminated bracket.
                return false;
            }
            case State::kBetween:
            {
                // Just closed a bracket — perfectly valid termination (e.g. "m_Components[2]").
                break;
            }
        }

        return !out_tokens.empty();
    }

    // =====================================================================================
    // SerialiseObject
    // -------------------------------------------------------------------------------------
    // One pass with GenerateTypeTreeTransfer is enough for Phase 2a: we use the resulting
    // TypeTree purely as a structural map, never indexing into a serialised byte stream.
    // =====================================================================================
    bool SerialiseObject(Object* object, SerialisedObject& out)
    {
        if (object == nullptr)
        {
            return false;
        }

        out.bytes.clear();

        // The 0 instruction-flags + nullptr/0 object-info combo matches what
        // TypeTreeCache::GetTypeTree uses internally for "describe this object's layout".
        GenerateTypeTreeTransfer typetree_pass(out.type_tree,
                                               kNoTransferInstructionFlags,
                                               static_cast<void*>(object),
                                               /*objectSize*/ 0);
        object->VirtualRedirectTransfer(typetree_pass);

        return true;
    }

    // =====================================================================================
    // LocateInTypeTree
    // -------------------------------------------------------------------------------------
    // Field tokens descend through Children/Next chains looking for a matching Name().
    //
    // Index tokens descend into a TypeTree array node's "data" subtree. The TypeTree
    // shape produced by GenerateTypeTreeTransfer for a vector<T> is:
    //
    //     <vector_node TypeFlags=kFlagIsArray>
    //       ├─ "size"   int    (1st child — children index 0)
    //       └─ "data"   <T>    (2nd child — children index 1, only emitted as a *type
    //                           descriptor*, since the array contents are repeated at
    //                           runtime). Index tokens always step here.
    // =====================================================================================
    TypeTreeIterator LocateInTypeTree(const TypeTree& type_tree, const std::vector<PathToken>& tokens)
    {
        if (tokens.empty())
        {
            return TypeTreeIterator();
        }

        // The TypeTree root is the object itself; children are the top-level fields.
        TypeTreeIterator current = type_tree.Root();

        for (const PathToken& token : tokens)
        {
            if (current.IsNull())
            {
                return TypeTreeIterator();
            }

            switch (token.kind)
            {
                case PathToken::Kind::Field:
                {
                    TypeTreeIterator scan_target = current;
                    bool found = false;

                    // For non-array nodes we iterate over Children(); for array nodes we
                    // only ever match field names against the *element type* subtree
                    // (because the array's logical fields live there).
                    if (current->IsArray())
                    {
                        // Skip directly to the "data" child's children — array index
                        // tokens are required before any field token can match.
                        return TypeTreeIterator();
                    }

                    for (TypeTreeIterator child = scan_target.Children(); !child.IsNull(); child = child.Next())
                    {
                        if (TypeTreeNameEquals(child.Name().c_str(), token.field))
                        {
                            current = child;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        return TypeTreeIterator();
                    }
                    break;
                }

                case PathToken::Kind::Index:
                {
                    if (!current->IsArray())
                    {
                        return TypeTreeIterator();
                    }
                    if (token.array_index < 0)
                    {
                        return TypeTreeIterator();
                    }
                    // Bound-check is by-design impossible here without the real `size`
                    // value (which lives in the byte stream). Phase 2b validates against
                    // the stream.

                    // First child: "size"; second child: "data". Step into "data".
                    TypeTreeIterator data_node = current.Children();
                    if (data_node.IsNull())
                    {
                        return TypeTreeIterator();
                    }
                    data_node = data_node.Next();  // skip past "size"
                    if (data_node.IsNull())
                    {
                        return TypeTreeIterator();
                    }
                    current = data_node;
                    break;
                }
            }
        }

        return current;
    }

    TypeTreeIterator LocateInTypeTree(const TypeTree& type_tree, const eastl::string& path)
    {
        std::vector<PathToken> tokens;
        if (!TokenizePropertyPath(path, tokens))
        {
            return TypeTreeIterator();
        }
        return LocateInTypeTree(type_tree, tokens);
    }

}  // namespace prefab_editor
