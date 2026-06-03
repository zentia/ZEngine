#include "SafeBinaryRead.h"

#include "Runtime/Utility/Align.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

CachedReader& SafeBinaryRead::Init(const TypeTreeIterator& oldBase, FileSize bytePosition, int64_t byteSize, TransferInstructionFlags flags)
{
    m_OldBaseType = oldBase;
    m_BaseBytePosition = bytePosition;
    m_BaseByteSize = byteSize;

    return m_Cache;
}

int SafeBinaryRead::BeginTransfer(const char* name, const char* typeString, ConversionFunction** converter, bool allowTypeConversion)
{
    if (converter != nullptr)
        *converter = nullptr;
    if (m_StackInfo.empty())
    {
        StackedInfo& newInfo = m_StackInfo.emplace_back();
        newInfo.type = m_OldBaseType;
        newInfo.bytePosition = m_BaseBytePosition;
        newInfo.version = 1;
        newInfo.currentTypeName = typeString;
        newInfo.cachedIterator = newInfo.type.Children();
        newInfo.cachedBytePosition = m_BaseBytePosition;

        m_CurrentStackInfo = &newInfo;

        return kMatchesType;
    }

    TypeTreeIterator c;

    StackedInfo& info = *m_CurrentStackInfo;

    TypeTreeIterator children = info.type.Children();

    FileSize newBytePosition = info.cachedBytePosition;
    int count = 0;
    for (c = info.cachedIterator; !c.IsNull(); c = c.Next())
    {
        if (c.Name() == name)
            break;
        Walk(c, &newBytePosition);
        count++;
    }

    // First-pass scan from cachedIterator may have missed: schema evolution
    // commonly walks past the field we want (we always linearly probe forward
    // from the previously-resolved cachedIterator). When that happens, restart
    // from the very first child of this frame and walk again. Mirrors Unity's
    // SafeBinaryRead.cpp `if (c.IsNull()) { ... }` second-pass block. Without
    // it, ANY schema-evolution field reorder (S4) and ANY new-field-on-old-
    // file (S3) would dereference a null TypeTreeIterator on the next line
    // below and access-violate.
    //
    // If even the second pass can't locate the name, return kNotFound. The
    // caller (TransferWithTypeString) checks for kNotFound and short-circuits,
    // leaving `data` at its default-constructed value -- which is exactly the
    // "added field default-initialised on read" semantic that PR-SE2 S3
    // proves.
    if (c.IsNull())
    {
        newBytePosition = info.bytePosition;
        for (c = children; !c.IsNull(); c = c.Next())
        {
            if (c.Name() == name)
                break;
            Walk(c, &newBytePosition);
        }

        if (c.IsNull())
        {
            return kNotFound;
        }
    }

    info.cachedIterator = c;
    info.cachedBytePosition = newBytePosition;

    if (info.type->IsArray() && c != children)
    {
        int32_t arrayPosition = *m_CurrentPositionInArray;

        if (c->m_ByteSize != -1 && (c->m_MetaFlag & (kAnyChildUsesAlignBytesFlag | kAlignBytesFlag)) == 0)
        {
            newBytePosition += (uint64_t)(c->m_ByteSize * arrayPosition);
        }
        else
        {
            ArrayPositionInfo& arrayInfo = m_PositionInArray.back();
            int32_t cachedArrayPosition = 0;
            if (arrayInfo.cachedArrayPosition <= arrayPosition)
            {
                newBytePosition = arrayInfo.cachedBytePosition;
                cachedArrayPosition = arrayInfo.cachedArrayPosition;
            }

            for (int32_t i = cachedArrayPosition; i < arrayPosition; i++)
                Walk(c, &newBytePosition);

            arrayInfo.cachedArrayPosition = arrayPosition;
            arrayInfo.cachedBytePosition = newBytePosition;
        }

        (*m_CurrentPositionInArray)++;
    }

    StackedInfo& newInfo = m_StackInfo.emplace_back();
    newInfo.type = c;
    newInfo.bytePosition = newBytePosition;
    newInfo.version = 1;
    newInfo.cachedIterator = newInfo.type.Children();
    newInfo.currentTypeName = typeString;
    // Seed cachedBytePosition to bytePosition so that the FIRST child lookup
    // inside this new frame starts walking from the correct file offset.
    // Without this, the default-constructed FileSize() == 0 makes child reads
    // dereference at absolute file offset 0 (= ZASS magic / junk), which
    // silently zeroes out string fields and any other variable-byte-size
    // child whose offset is computed by walking from cachedBytePosition.
    // Mirrors Unity's SafeBinaryRead.cpp BeginTransfer (Stage 2 reference).
    newInfo.cachedBytePosition = newBytePosition;

    m_CurrentStackInfo = &newInfo;

    int conversion = kNeedConversion;

    if (c.Type() == typeString || allowTypeConversion || m_StackInfo.size() == 1)
    {
        conversion = kMatchesType;
        if (c.ByteSize() != -1 && (c.MetaFlags() & (kAnyChildUsesAlignBytesFlag | kAlignBytesFlag)) == 0)
        {
            conversion = kFastPathKnownByteSizeArrayType;
        }
    }
    return conversion;
}

void SafeBinaryRead::EndTransfer()
{
    m_StackInfo.pop_back();
    if (!m_StackInfo.empty())
    {
        m_CurrentStackInfo = &m_StackInfo.back();
    }
    m_DidReadLastProperty = true;
}

bool SafeBinaryRead::BeginArrayTransfer(const char* name, const char* typeString, int32_t& size)
{
    if (BeginTransfer(name, typeString, nullptr, false) == kNotFound)
        return false;

    Transfer(size, "size");
    ArrayPositionInfo info;
    info.arrayPosition = 0;
    info.cachedBytePosition = FileSize();
    info.cachedArrayPosition = std::numeric_limits<int32_t>::max();

    m_PositionInArray.push_back(info);
    m_CurrentPositionInArray = &m_PositionInArray.back().arrayPosition;
    return true;
}

void SafeBinaryRead::EndArrayTransfer()
{
    m_PositionInArray.pop_back();
    if (!m_PositionInArray.empty())
        m_CurrentPositionInArray = &m_PositionInArray.back().arrayPosition;
    else
    {
        m_CurrentPositionInArray = nullptr;
    }

    EndTransfer();
}

void SafeBinaryRead::Walk(const TypeTreeIterator& type, FileSize* bytePosition)
{
    if (type->m_ByteSize != -1 && (type->m_MetaFlag & kAnyChildUsesAlignBytesFlag) == 0)
    {
        *bytePosition += (uint64_t)type->m_ByteSize;
    }
    else if (type->IsArray())
    {
        int32_t arraySize, i;
        m_Cache.Read(arraySize, bytePosition->Cast<size_t>());

        *bytePosition += (uint64_t)sizeof(arraySize);

        TypeTreeIterator elementTypeTree = type.Children().Next();

        if (elementTypeTree->m_ByteSize != -1 && (elementTypeTree->m_MetaFlag & (kAnyChildUsesAlignBytesFlag | kAlignBytesFlag)) == 0)
        {
            *bytePosition += (uint64_t)(arraySize * elementTypeTree->m_ByteSize);
        }
        else
        {
            for (i = 0; i < arraySize; ++i)
            {
                Walk(elementTypeTree, bytePosition);
            }
        }
    }
    else
    {
        for (TypeTreeIterator i = type.Children(); !i.IsNull(); i = i.Next())
        {
            Walk(i, bytePosition);
        }
    }

    if (type->m_MetaFlag & kAlignBytesFlag)
    {
        *bytePosition = AlignToPowerOfTwo<uint64_t, uint64_t>(bytePosition->Cast<uint64_t>(), 4);
    }
}