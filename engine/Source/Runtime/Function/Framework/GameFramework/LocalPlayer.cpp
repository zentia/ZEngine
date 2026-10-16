#include "LocalPlayer.h"
#include "PlayerController.h"

#include "Runtime/Core/Base/Macro.h"

namespace
{
    constexpr int k_invalid_player_index = -1;
}  // namespace

LocalPlayer::LocalPlayer(int index)
    : m_Index(index)
{
}

LocalPlayer::~LocalPlayer()
{
    Shutdown();
}

bool LocalPlayer::Initialize()
{
    if (m_IsInitialized)
    {
        return true;
    }

    if (m_Index == k_invalid_player_index)
    {
        LOG_ERROR(ZEngine, "LocalPlayer: refusing to initialize with an invalid index");
        return false;
    }

    if (m_PlayerController == nullptr)
    {
        m_PlayerController = new PlayerController(this);
    }

    m_IsInitialized = true;
    LOG_INFO(ZEngine, "LocalPlayer[{}] initialized", m_Index);
    return true;
}

void LocalPlayer::Shutdown()
{
    if (!m_IsInitialized)
    {
        return;
    }

    if (m_PlayerController != nullptr)
    {
        delete m_PlayerController;
        m_PlayerController = nullptr;
    }

    m_IsInitialized = false;
    LOG_INFO(ZEngine, "LocalPlayer[{}] shut down", m_Index);
}

void LocalPlayer::Tick(float delta_time)
{
    if (m_PlayerController != nullptr)
    {
        m_PlayerController->Tick(delta_time);
    }
}

void LocalPlayer::SetViewport(int x, int y, int width, int height)
{
    m_ViewportX = x;
    m_ViewportY = y;
    m_ViewportWidth = width;
    m_ViewportHeight = height;
}
