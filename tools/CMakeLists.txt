# =============================================================================
# Standalone ZEngine tools that do NOT depend on the engine
# =============================================================================
# Added from the root CMakeLists.txt AFTER add_subdirectory(engine). Do not
# move it earlier: every tool here links ZSlate, which the engine defines. If
# ZSlate is not yet a target, a tool creates its own copy with
# add_subdirectory(engine/Libraries/ZSlate) and the engine's later add fails
# with "add_library cannot create target ZSlate because another target with the
# same name already exists".
#
# Tools that DO need engine internals (ZRuntime decompressors, SerializedFile,
# the asset registry) are NOT here -- they live in engine/Source/Tools/ and are
# added by engine/CMakeLists.txt. See AGENTS.md section 8.

option(ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK "Build the standalone Windows environment checker" ON)

if(ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK AND WIN32)
    # The CLI part (ZWindowsEnvironmentCheck, ZDllLoadProbe) needs no ZSlate and
    # builds fine here. Its two ZSlate GUIs default to OFF in a full-tree
    # configure (ENVIRONMENT_CHECK_BUILD_GUI) and that default is deliberate --
    # do not "fix" it:
    #
    # WindowsEnvironmentCheck sets CMAKE_MSVC_RUNTIME_LIBRARY to /MT because it
    # ships to machines with no VC redist. Standalone, ZSlate is added inside
    # that scope and inherits /MT. In a full-tree configure ZSlate already
    # exists as /MD (engine default), so linking it into a /MT collector fails
    # with LNK2038 (RuntimeLibrary mismatch). One ZSlate instance cannot be
    # both. Build the collector GUI from its own configure:
    #     cmake -S tools/WindowsEnvironmentCheck -B <dir>
    add_subdirectory(WindowsEnvironmentCheck)
endif()

# ClientProjBranches is a self-contained CMake project: it configures and
# builds fine on its own (cmake -S . -B build), which is how it is normally
# used. It is OFF here by default so a full engine configure does not pick up a
# second, unrelated tool with its own C++ standard and output layout.
option(ZENGINE_BUILD_CLIENT_PROJ_BRANCHES "Build the ClientProjBranches branch/stack-trace viewer" OFF)

if(ZENGINE_BUILD_CLIENT_PROJ_BRANCHES AND WIN32)
    add_subdirectory(ClientProjBranches)
endif()
