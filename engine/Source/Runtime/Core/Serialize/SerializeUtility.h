#pragma once

#include <type_traits>

class StreamedBinaryRead;

/// True for readers that look fields up BY NAME (SafeBinaryRead through the file's
/// TypeTree, YAMLRead, JSONRead): a Transfer() of a name that is not in the data
/// just leaves the value untouched, so a reading Transfer() may PROBE legacy field
/// names. False for everything else, which must see exactly the writer's fields in
/// the writer's order:
///   * StreamedBinaryRead is positional -- an extra or reordered Transfer() reads
///     the wrong bytes. TransferUtility::CloneObjectViaSerialization (prefab
///     instantiate), the prefab override snapshots and Undo snapshots go through it;
///     a misread string length there is an access violation, not a wrong value.
///   * GenerateTypeTreeTransfer (neither reading nor writing) DESCRIBES the written
///     data; legacy entries in it shift every later offset for SafeBinaryRead.
/// Write the legacy probes as `if constexpr (kTransferProbesLegacyFields<T>)` and
/// give every other transfer the writer's sequence.
template<typename TransferFunction>
inline constexpr bool kTransferProbesLegacyFields =
    TransferFunction::IsReading() && !std::is_same_v<TransferFunction, StreamedBinaryRead>;

#define TRANSFER_WITH_NAME(x, name) transfer.Transfer(x, name)
#define TRANSFER(x)                 TRANSFER_WITH_NAME(x, #x)

#define DECLARE_SERIALIZE(x)                       \
    inline static const char* GetTypeString()      \
    {                                              \
        return #x;                                 \
    }                                              \
    inline static bool AllowTransferOptimization() \
    {                                              \
        return false;                              \
    }                                              \
    template<class TransferFunction>               \
    void Transfer(TransferFunction& transfer);