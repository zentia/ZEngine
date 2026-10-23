#pragma once

class ConsoleManager;

// UE-style built-in runtime commands and CVars (registered from ConsoleManager::Initialize).
void RegisterRuntimeConsoleCommands(ConsoleManager& console);

// `exec.after <frames> <command line>` 排的延迟命令：每帧调一次（Application::TickOneFrame），到期的按排队顺序执行。
// 给无人值守的验证跑用：启动参数里的 --cvar 都在第一帧之前执行，要"关卡加载完 / 对象建好之后再做某事"只能延后。
void TickDeferredConsoleCommands();
