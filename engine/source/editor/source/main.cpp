#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>

#include "runtime/engine.h"

#include "editor/include/editor.h"

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define ZENTIA_XSTR(s) ZENTIA_STR(s)
#define ZENTIA_STR(s) #s

int main(int argc, char** argv)
{
    std::filesystem::path executable_path(argv[0]);
    std::filesystem::path config_file_path = executable_path.parent_path() / "ZEditor.ini";

    Z::ZentiaEngine* engine = new Z::ZentiaEngine();

    engine->startEngine(config_file_path.generic_string());
    engine->initialize();

    Z::ZentiaEditor* editor = new Z::ZentiaEditor();
    editor->initialize(engine);

    editor->run();

    editor->clear();

    engine->clear();
    engine->shutdownEngine();

    return 0;
}
