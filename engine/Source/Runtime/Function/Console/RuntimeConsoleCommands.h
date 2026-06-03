#pragma once

class ConsoleManager;

// UE-style built-in runtime commands and CVars (registered from ConsoleManager::Initialize).
void RegisterRuntimeConsoleCommands(ConsoleManager& console);
