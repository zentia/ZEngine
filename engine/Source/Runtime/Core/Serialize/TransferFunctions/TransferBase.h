#pragma once

#include "Runtime/Core/Serialize/SerializeTraits.h"

extern const char* kTransferNameIdentifierBase;

class TransferBase
{
public:
    void Align() {}

protected:
    TransferInstructionFlags m_Flags;
};