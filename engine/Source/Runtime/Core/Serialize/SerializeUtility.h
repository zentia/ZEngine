#pragma once

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