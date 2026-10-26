# =====================================================================
# Physics P0-2 smoke test: character controller + triggers + joints
# ---------------------------------------------------------------------
# Headless -- no WorldManager, no RHI, no level. It builds a bare
# PhysicsScene and drives Jolt directly, which is enough to prove the
# CharacterVirtual wiring, the sensor flag and every exported joint type.
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_PHYSICS_CHARACTER_SMOKE_TEST=ON
#   cmake --build build --target PhysicsCharacterJointSmokeTest
#   ctest -R PhysicsCharacterJointSmokeTest
# =====================================================================

option(ZENGINE_BUILD_PHYSICS_CHARACTER_SMOKE_TEST
       "Build the physics character / trigger / joint smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_PHYSICS_CHARACTER_SMOKE_TEST)
    return()
endif()

add_executable(PhysicsCharacterJointSmokeTest PhysicsCharacterJointSmokeTest.cpp)

set_target_properties(PhysicsCharacterJointSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Links ZRuntime for EASTL + math types; the physics translation units come in
# through /WHOLEARCHIVE on the static lib, same as the other smoke tests.
target_link_libraries(PhysicsCharacterJointSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(PhysicsCharacterJointSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(PhysicsCharacterJointSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(PhysicsCharacterJointSmokeTest ZEnginePCH)
    target_precompile_headers(PhysicsCharacterJointSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(PhysicsCharacterJointSmokeTest PRIVATE /W4)
endif()

add_test(NAME PhysicsCharacterJointSmokeTest COMMAND PhysicsCharacterJointSmokeTest)

message(STATUS "Configured physics character smoke-test target: PhysicsCharacterJointSmokeTest")
