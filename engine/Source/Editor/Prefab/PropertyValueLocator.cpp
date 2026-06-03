#include "Editor/Prefab/PropertyValueLocator.h"

#include "Runtime/Utility/Align.h"

#include <cstring>

namespace prefab_editor
{

    namespace
    {

        // Read a 32-bit little-endian / native-int value from the stream at `pos`. Returns
        // false if `pos + 4 > bytes.size()`. The StreamedBinary writer (CachedWriter::Write)
        // dumps host-endian raw bytes, which on every platform we ship today is LE. If we
        // ever support BE, this is one of two sites that need swap helpers (the other being
        // SafeBinaryRead's read path).
        bool ReadInt32(const std::vector<uint8_t>& bytes, size_t pos, int32_t& out)
        {
            if (pos + sizeof(int32_t) > bytes.size())
                return false;
            std::memcpy(&out, bytes.data() + pos, sizeof(int32_t));
            return true;
        }

        /// Walk `node` over `bytes`, advancing `*pos` past the bytes the node occupies.
        /// Returns false on a malformed stream (truncated array sizes, etc.). Modeled on
        /// SafeBinaryRead::Walk:
        ///   - Fixed-size primitive: bump `*pos` by m_ByteSize.
        ///   - Array: read int32 size prefix, then either fast-path multiply by element
        ///     ByteSize (when the element is fixed-size and not align-flagged) or recurse
        ///     per-element.
        ///   - Struct: recurse into each child.
        ///   - Apply 4-byte align padding when the node has kAlignBytesFlag.
        bool WalkPastNode(const TypeTreeIterator& node, const std::vector<uint8_t>& bytes, size_t& pos)
        {
            const TypeTreeNode* n = node.operator->();
            if (n == nullptr)
                return false;

            const bool any_child_align = (n->m_MetaFlag & kAnyChildUsesAlignBytesFlag) != 0;

            if (n->m_ByteSize != -1 && !any_child_align)
            {
                pos += static_cast<size_t>(n->m_ByteSize);
            }
            else if (node->IsArray())
            {
                int32_t size = 0;
                if (!ReadInt32(bytes, pos, size))
                    return false;
                pos += sizeof(int32_t);

                // First child is "size" (already consumed above), second is the element
                // type descriptor.
                TypeTreeIterator size_child = node.Children();
                if (size_child.IsNull())
                    return false;
                TypeTreeIterator data_child = size_child.Next();
                if (data_child.IsNull())
                    return false;

                const bool elem_any_child_align = (data_child->m_MetaFlag & (kAnyChildUsesAlignBytesFlag | kAlignBytesFlag)) != 0;
                if (data_child->m_ByteSize != -1 && !elem_any_child_align)
                {
                    pos += static_cast<size_t>(size) * static_cast<size_t>(data_child->m_ByteSize);
                }
                else
                {
                    for (int32_t i = 0; i < size; ++i)
                    {
                        if (!WalkPastNode(data_child, bytes, pos))
                            return false;
                    }
                }
            }
            else
            {
                for (TypeTreeIterator c = node.Children(); !c.IsNull(); c = c.Next())
                {
                    if (!WalkPastNode(c, bytes, pos))
                        return false;
                }
            }

            if (n->m_MetaFlag & kAlignBytesFlag)
                pos = AlignToPowerOfTwo<uint64_t, uint64_t>(pos, 4);

            return true;
        }

        /// Like WalkPastNode but:
        ///   - Stops when the path token chain matches the leaf currently being visited,
        ///     filling `out` and returning kHit.
        ///   - Returns kMiss when the recursion is "passing by" — caller should bump
        ///     position and continue.
        ///   - Returns kError on stream corruption / mismatch.
        enum class StepResult
        {
            kHit,
            kMiss,
            kError,
        };

        StepResult Descend(
            const TypeTreeIterator& node,
            const std::vector<uint8_t>& bytes,
            const std::vector<PathToken>& tokens,
            size_t token_index,
            size_t& pos,
            PropertyLocation& out);

        /// Helper: advance through the children of `parent` looking for a Field-token match,
        /// while WalkPastNode-ing every non-matching sibling.
        StepResult DescendIntoStruct(
            const TypeTreeIterator& parent,
            const std::vector<uint8_t>& bytes,
            const std::vector<PathToken>& tokens,
            size_t token_index,
            size_t& pos,
            PropertyLocation& out)
        {
            if (token_index >= tokens.size())
                return StepResult::kError;  // ran out of tokens but still descending — caller bug
            const PathToken& tok = tokens[token_index];
            if (tok.kind != PathToken::Kind::Field)
                return StepResult::kError;

            for (TypeTreeIterator c = parent.Children(); !c.IsNull(); c = c.Next())
            {
                const TypeTreeString name = c.Name();
                if (name == tok.field.c_str())
                {
                    // Match: descend.
                    return Descend(c, bytes, tokens, token_index + 1, pos, out);
                }
                if (!WalkPastNode(c, bytes, pos))
                    return StepResult::kError;
            }
            return StepResult::kMiss;  // field not found in this struct
        }

        /// Helper: advance into an array, looking for an Index-token match. Reads array
        /// size from the stream and validates `[i]` is in range.
        StepResult DescendIntoArray(
            const TypeTreeIterator& array_node,
            const std::vector<uint8_t>& bytes,
            const std::vector<PathToken>& tokens,
            size_t token_index,
            size_t& pos,
            PropertyLocation& out)
        {
            if (token_index >= tokens.size())
                return StepResult::kError;
            const PathToken& tok = tokens[token_index];
            if (tok.kind != PathToken::Kind::Index)
                return StepResult::kError;  // path expected an index here

            int32_t size = 0;
            if (!ReadInt32(bytes, pos, size))
                return StepResult::kError;
            pos += sizeof(int32_t);

            if (tok.array_index < 0 || tok.array_index >= size)
                return StepResult::kMiss;

            TypeTreeIterator size_child = array_node.Children();
            if (size_child.IsNull())
                return StepResult::kError;
            TypeTreeIterator data_child = size_child.Next();
            if (data_child.IsNull())
                return StepResult::kError;

            // Skip preceding elements.
            for (int32_t i = 0; i < tok.array_index; ++i)
            {
                if (!WalkPastNode(data_child, bytes, pos))
                    return StepResult::kError;
            }
            // Now `pos` is at the start of the requested element.
            return Descend(data_child, bytes, tokens, token_index + 1, pos, out);
        }

        StepResult Descend(
            const TypeTreeIterator& node,
            const std::vector<uint8_t>& bytes,
            const std::vector<PathToken>& tokens,
            size_t token_index,
            size_t& pos,
            PropertyLocation& out)
        {
            // Path consumed: this node is the leaf.
            if (token_index == tokens.size())
            {
                out.byte_offset = pos;
                out.leaf_node = node;

                const TypeTreeNode* n = node.operator->();
                const bool any_child_align = n != nullptr ? ((n->m_MetaFlag & kAnyChildUsesAlignBytesFlag) != 0) : false;
                if (n != nullptr && n->m_ByteSize != -1 && !any_child_align)
                {
                    out.byte_size = n->m_ByteSize;
                }
                else
                {
                    // Variable-size leaf: caller can still read sub-fields, but a flat
                    // OverwriteScalarBytes won't work. Walk past the node to learn its
                    // span anyway, so callers can splice instead of overwrite.
                    size_t end_pos = pos;
                    if (!WalkPastNode(node, bytes, end_pos))
                        return StepResult::kError;
                    out.byte_size = static_cast<int64_t>(end_pos) - static_cast<int64_t>(pos);
                }
                return StepResult::kHit;
            }

            // More tokens remain: dispatch on what kind we expect next.
            if (node->IsArray())
                return DescendIntoArray(node, bytes, tokens, token_index, pos, out);
            return DescendIntoStruct(node, bytes, tokens, token_index, pos, out);
        }

    }  // namespace

    bool LocatePropertyInStream(
        const TypeTree& type_tree,
        const std::vector<uint8_t>& bytes,
        const std::vector<PathToken>& tokens,
        PropertyLocation& out)
    {
        out = PropertyLocation();
        if (tokens.empty())
            return false;

        TypeTreeIterator root = type_tree.Root();
        if (root.IsNull())
            return false;

        size_t pos = 0;
        const StepResult result = Descend(root, bytes, tokens, 0, pos, out);
        return result == StepResult::kHit;
    }

    bool LocatePropertyInStream(
        const TypeTree& type_tree,
        const std::vector<uint8_t>& bytes,
        const eastl::string& path,
        PropertyLocation& out)
    {
        std::vector<PathToken> tokens;
        if (!TokenizePropertyPath(path, tokens))
            return false;
        return LocatePropertyInStream(type_tree, bytes, tokens, out);
    }

    bool OverwriteScalarBytes(
        std::vector<uint8_t>& out_bytes,
        const PropertyLocation& loc,
        const void* new_value,
        size_t new_value_size)
    {
        if (!loc.valid())
            return false;
        if (loc.byte_size != static_cast<int64_t>(new_value_size))
            return false;
        if (loc.byte_offset + new_value_size > out_bytes.size())
            return false;

        std::memcpy(out_bytes.data() + loc.byte_offset, new_value, new_value_size);
        return true;
    }

}  // namespace prefab_editor
