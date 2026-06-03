#pragma once

class ConsoleManager;

// Editor-only console commands (asset registry, level, play mode). Call from Editor::Initialize.
void RegisterEditorConsoleCommands(ConsoleManager& console);
