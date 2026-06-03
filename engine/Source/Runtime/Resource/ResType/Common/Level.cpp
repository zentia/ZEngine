#include "Level.h"

IMPLEMENT_REGISTER_CLASS(LevelRes)
IMPLEMENT_OBJECT_SERIALIZE(LevelRes)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(LevelRes)

template<typename TransferFunction>
void LevelRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Gravity, "gravity");
    transfer.Transfer(m_CharacterName, "character_name");
    // transfer.Transfer(m_Objects, "objects");
}