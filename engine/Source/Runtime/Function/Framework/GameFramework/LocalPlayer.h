#pragma once

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <string>

class PlayerController;

/// UE ULocalPlayer 对应物：一台机器上的一个真人玩家。
///
/// 分屏时每个玩家一份，各自持有独立的 PlayerController 与视口。
/// 单人游戏里实例只有一个，但代码路径保持一致，避免以后加分屏时重写。
///
/// 与 PlayerController 的关系（照 UE）：
///   LocalPlayer 是"座位"，PlayerController 是"坐在座位上的人"。
///   座位在进程启动时创建并一直存在；PlayerController 可以随关卡重建。
class LocalPlayer
{
public:
    explicit LocalPlayer(int index);
    virtual ~LocalPlayer();

    /// 分配 PlayerController。关卡加载后由 GameEngine 调用。
    bool Initialize();
    void Shutdown();
    void Tick(float delta_time);

    int GetIndex() const { return m_Index; }
    bool IsInitialized() const { return m_IsInitialized; }

    PlayerController* GetPlayerController() const { return m_PlayerController; }

    /// 视口尺寸（分屏时每个玩家只占窗口的一部分）。
    /// 骨架阶段固定为全窗口，由 GameEngine 在窗口尺寸变化时写入。
    void SetViewport(int x, int y, int width, int height);
    int GetViewportX() const { return m_ViewportX; }
    int GetViewportY() const { return m_ViewportY; }
    int GetViewportWidth() const { return m_ViewportWidth; }
    int GetViewportHeight() const { return m_ViewportHeight; }

protected:
    int m_Index {0};
    bool m_IsInitialized {false};

    PlayerController* m_PlayerController {nullptr};

    int m_ViewportX {0};
    int m_ViewportY {0};
    int m_ViewportWidth {0};
    int m_ViewportHeight {0};
};
