#pragma once

#include "Runtime/Utility/EnumFlags.h"

#include <cstdint>
#include <type_traits>

enum TransferMetaFlags : uint32_t
{
    kNoTransferFlags = 0u,
    HideInEditorMask = 1u << 0,
    Deprecated = 1u << 1,
    Packed = 1u << 2,
    StrongPPtrMask = 1 << 6,
    kTreatIntegerValueAsBoolean = 1 << 8,
    kAlignBytesFlag = 1 << 14,
    kAnyChildUsesAlignBytesFlag = 1 << 15,
    kTransferHex64 = 1 << 24,
    DisallowSerializedPropertyModification = 1 << 28
};
ENUM_FLAGS(TransferMetaFlags)

enum TransferInstructionFlags : uint64_t
{
    kNoTransferInstructionFlags = 0u,
    kReadWriteFromSerializedFile = 1ull << 0,
    kDontRequireAllMetaFlags = 1ull << 13
};