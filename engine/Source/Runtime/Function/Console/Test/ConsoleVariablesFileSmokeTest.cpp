// =====================================================================
// ConsoleVariablesFileSmokeTest
// ---------------------------------------------------------------------
// ConsoleManager::SaveVariablesToFile / LoadVariablesFromFile (the
// "cvar.save" / "cvar.load" commands, UE ConsoleVariables.ini style).
//
//   C1 save -> change -> load restores every type (int/float/bool/string)
//   C2 output is name=value, sorted, one per line, with a header comment
//   C3 only_changed_from snapshot writes just the changed variables
//   C4 comments (#, ;), [Section] lines, blank lines, CRLF, spaces around '='
//      and quoted values are accepted; names are case-insensitive
//   C5 unknown variable / invalid value / malformed line are skipped and
//      reported, later lines still apply
//   C6 string values with edge blanks or surrounding quotes round-trip exactly
//   C7 missing file -> -1; the console commands work end-to-end
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt. Pure file
// work, no project / window / GPU.
// =====================================================================

#include "Runtime/Function/Console/ConsoleManager.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
            ++g_Failures;
        std::printf("  [%s] %s\n", condition ? " ok " : "FAIL", name);
    }

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void WriteAll(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    struct Vars
    {
        std::shared_ptr<ConsoleVariable<int>> i;
        std::shared_ptr<ConsoleVariable<float>> f;
        std::shared_ptr<ConsoleVariable<bool>> b;
        std::shared_ptr<ConsoleVariable<std::string>> s;
    };

    Vars RegisterTestVariables(ConsoleManager& console)
    {
        Vars v;
        v.i = console.RegisterIntVariable("t.Count", "test int", 3);
        v.f = console.RegisterFloatVariable("t.Scale", "test float", 0.5f);
        v.b = console.RegisterBoolVariable("t.Enable", "test bool", false);
        v.s = console.RegisterStringVariable("t.Name", "test string", "alpha");
        return v;
    }

    void SetEnv(const char* name, const char* value)
    {
#ifdef _WIN32
        _putenv_s(name, value != nullptr ? value : "");
#else
        if (value != nullptr)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    void WriteFile(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
}  // namespace

int main()
{
    std::printf("ConsoleVariablesFileSmokeTest\n");
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "zengine_cvar_smoke";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string file = (dir / "ConsoleVariables.ini").generic_string();

    ConsoleManager console;
    Vars v = RegisterTestVariables(console);

    // ---- C1 / C2 ---------------------------------------------------------------
    std::printf("C1/C2 save -> change -> load\n");
    v.i->SetValue(42);
    v.f->SetValue(2.25f);
    v.b->SetValue(true);
    v.s->SetValue("hello world");
    const int written = console.SaveVariablesToFile(file);
    Check(written == 4, "C1 save writes the 4 registered variables");

    const std::string text = ReadAll(file);
    Check(text.rfind(";", 0) == 0, "C2 file starts with a ';' header comment");
    const size_t p_count = text.find("t.Count=42\n");
    const size_t p_enable = text.find("t.Enable=1\n");
    const size_t p_name = text.find("t.Name=hello world\n");
    const size_t p_scale = text.find("t.Scale=");
    Check(p_count != std::string::npos && p_enable != std::string::npos && p_name != std::string::npos &&
              p_scale != std::string::npos,
          "C2 one name=value line per variable, original-case names");
    Check(p_count < p_enable && p_enable < p_name && p_name < p_scale, "C2 lines sorted by name");

    v.i->SetValue(0);
    v.f->SetValue(0.0f);
    v.b->SetValue(false);
    v.s->SetValue("changed");
    const int applied = console.LoadVariablesFromFile(file);
    Check(applied == 4, "C1 load applies 4 values");
    Check(v.i->getValue() == 42, "C1 int restored");
    Check(v.f->getValue() == 2.25f, "C1 float restored");
    Check(v.b->getValue(), "C1 bool restored");
    Check(v.s->getValue() == "hello world", "C1 string (inner space) restored");

    // ---- C3 --------------------------------------------------------------------
    std::printf("C3 only-changed snapshot\n");
    const auto snapshot = console.SnapshotVariableValues();
    v.i->SetValue(7);
    const std::string changed_file = (dir / "changed.ini").generic_string();
    Check(console.SaveVariablesToFile(changed_file, &snapshot) == 1, "C3 one variable changed -> one line");
    const std::string changed = ReadAll(changed_file);
    Check(changed.find("t.Count=7\n") != std::string::npos && changed.find("t.Scale") == std::string::npos,
          "C3 file holds only the changed variable");

    // ---- C4 --------------------------------------------------------------------
    std::printf("C4 tolerant parsing\n");
    const std::string lenient = (dir / "lenient.ini").generic_string();
    WriteAll(lenient,
             "# hash comment\r\n"
             "; semicolon comment\r\n"
             "[Startup]\r\n"
             "\r\n"
             "  T.COUNT  =  11  \r\n"
             "t.scale=1.5\r\n"
             "t.enable = 0\r\n"
             "t.name = \"quoted value\"\r\n");
    std::vector<std::string> failed;
    Check(console.LoadVariablesFromFile(lenient, &failed) == 4, "C4 4 values applied");
    Check(failed.empty(), "C4 nothing reported as failed");
    Check(v.i->getValue() == 11, "C4 case-insensitive name, blanks around '=' and value");
    Check(v.f->getValue() == 1.5f, "C4 float");
    Check(!v.b->getValue(), "C4 bool");
    Check(v.s->getValue() == "quoted value", "C4 surrounding quotes stripped");

    // ---- C5 --------------------------------------------------------------------
    std::printf("C5 bad lines are skipped, not fatal\n");
    const std::string bad = (dir / "bad.ini").generic_string();
    WriteAll(bad,
             "t.Missing=1\n"
             "t.Count=notanumber\n"
             "no equals sign here\n"
             "=5\n"
             "t.Count=99\n");
    failed.clear();
    Check(console.LoadVariablesFromFile(bad, &failed) == 1, "C5 only the good line applies");
    Check(failed.size() == 4, "C5 four bad lines reported");
    Check(v.i->getValue() == 99, "C5 the line after the bad ones still applied");

    // setStringValue used std::stoi / std::stof: a trailing-garbage value was taken
    // as its numeric prefix, and with exception catching off (web) a non-number threw
    // straight through the catch and aborted.
    Check(!v.i->setStringValue("12abc") && v.i->getValue() == 99, "C5 int: trailing garbage rejected, value kept");
    Check(!v.f->setStringValue("1.5x"), "C5 float: trailing garbage rejected");
    Check(!v.i->setStringValue("") && !v.i->setStringValue("   "), "C5 int: empty / blank rejected");
    Check(v.i->setStringValue("+8") && v.i->getValue() == 8, "C5 int: leading '+' accepted");
    Check(v.i->setStringValue("-3") && v.i->getValue() == -3, "C5 int: negative accepted");
    Check(!v.i->setStringValue("99999999999"), "C5 int: out-of-range rejected");

    // ---- C6 --------------------------------------------------------------------
    std::printf("C6 awkward string values round-trip\n");
    const char* awkward[] = {"  leading", "trailing  ", "\"already quoted\"", "", "a=b;c#d"};
    for (const char* value : awkward)
    {
        v.s->SetValue(value);
        const std::string f6 = (dir / "awkward.ini").generic_string();
        console.SaveVariablesToFile(f6);
        v.s->SetValue("sentinel");
        console.LoadVariablesFromFile(f6);
        char name[96];
        std::snprintf(name, sizeof(name), "C6 round-trips [%s]", value);
        Check(v.s->getValue() == value, name);
    }

    // ---- C7 --------------------------------------------------------------------
    std::printf("C6b floats round-trip bit-exactly\n");
    const float awkward_floats[] = {1e-7f, 0.1f, 123456.789f, -3.0f, 1.0f / 3.0f};
    for (float value : awkward_floats)
    {
        v.f->SetValue(value);
        const std::string f6b = (dir / "floats.ini").generic_string();
        console.SaveVariablesToFile(f6b);
        v.f->SetValue(-999.0f);
        console.LoadVariablesFromFile(f6b);
        char name[96];
        std::snprintf(name, sizeof(name), "C6b float %.9g round-trips (saved as %s)", value, v.f->getStringValue().c_str());
        Check(v.f->getValue() == value, name);
    }

    // ---- C7 --------------------------------------------------------------------
    std::printf("C7 errors and commands\n");
    Check(console.LoadVariablesFromFile((dir / "does_not_exist.ini").generic_string()) == -1,
          "C7 missing file -> -1");
    // Commands are registered by Initialize(); use a second manager for them so
    // RegisterRuntimeConsoleCommands' own variables do not change C1's count.
    ConsoleManager full;
    full.Initialize();
    auto cmd_var = full.RegisterIntVariable("t.CmdValue", "command test", 1);
    const std::string cmd_file = (dir / "cmd.ini").generic_string();
    cmd_var->SetValue(123);
    Check(full.ExecuteString("cvar.save " + cmd_file), "C7 'cvar.save <file>' succeeds");
    cmd_var->SetValue(5);
    Check(full.ExecuteString("cvar.load " + cmd_file), "C7 'cvar.load <file>' succeeds");
    Check(cmd_var->getValue() == 123, "C7 value restored through the commands");
    Check(!full.ExecuteString("cvar.load"), "C7 'cvar.load' without a file fails");
    full.Shutdown();

    // ---- C8 --------------------------------------------------------------------
    // Startup file (UE Config/ConsoleVariables.ini): default location, overrides,
    // disabling, missing file is not an error, the env var, --cvar wins.
    std::printf("C8 startup cvar file\n");
    {
        const std::filesystem::path project = dir / "proj";
        const std::filesystem::path expected = project / "Config" / "ConsoleVariables.ini";
        Check(ConsoleManager::ResolveStartupVariablesFile(project, nullptr) == expected, "C8 default path");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "") == expected, "C8 empty spec -> default path");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "  ") == expected, "C8 blank spec -> default path");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "none").empty(), "C8 'none' disables");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "OFF").empty(), "C8 'OFF' disables (any case)");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "0").empty(), "C8 '0' disables");
        Check(ConsoleManager::ResolveStartupVariablesFile(project, "my.ini") == std::filesystem::path("my.ini"),
              "C8 anything else is a path");

        ConsoleManager boot;
        auto count = boot.RegisterIntVariable("t.Count", "startup test", 3);
        Check(boot.LoadStartupVariablesFile(expected) == 0 && count->getValue() == 3,
              "C8 no file at the default location -> 0, nothing changes (not an error)");
        Check(boot.LoadStartupVariablesFile({}) == 0 && count->getValue() == 3, "C8 disabled -> 0, nothing changes");

        WriteFile(expected, "[Startup]\nt.Count=77\n");
        Check(boot.LoadStartupVariablesFile(ConsoleManager::ResolveStartupVariablesFile(project, nullptr)) == 1 &&
                  count->getValue() == 77,
              "C8 default file applied");

        const std::filesystem::path other = dir / "other.ini";
        WriteFile(other, "t.Count=55\n");
        Check(boot.LoadStartupVariables(other.generic_string().c_str()) == 1 && count->getValue() == 55,
              "C8 explicit --cvar-file path applied");
        count->SetValue(3);
        Check(boot.LoadStartupVariables("none") == 0 && count->getValue() == 3, "C8 --cvar-file none -> nothing applied");

        SetEnv(ConsoleManager::k_startup_variables_env, other.generic_string().c_str());
        Check(boot.LoadStartupVariables() == 1 && count->getValue() == 55, "C8 ZENGINE_CVAR_FILE picked up");
        count->SetValue(3);
        Check(boot.LoadStartupVariables("none") == 0 && count->getValue() == 3,
              "C8 an explicit spec wins over ZENGINE_CVAR_FILE");
        SetEnv(ConsoleManager::k_startup_variables_env, nullptr);

        // Startup order used by the editor / ZGame: file first, then --cvar.
        boot.LoadStartupVariables(other.generic_string().c_str());
        Check(boot.ExecuteString("t.Count=9") && count->getValue() == 9, "C8 a later --cvar overrides the file value");
    }

    std::filesystem::remove_all(dir, ec);
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
