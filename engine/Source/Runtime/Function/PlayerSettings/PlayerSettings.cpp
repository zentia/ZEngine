#include "PlayerSettings.h"

bool PlayerSettings::Initialize()
{
    m_ProjectName = GET_SYSTEM(ProjectInfo)->name.c_str();
    return true;
}