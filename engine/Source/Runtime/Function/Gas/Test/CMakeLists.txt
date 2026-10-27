# =====================================================================
# GAS 冷却时间冒烟测试（GAS 冷却时间落地验证）
# ---------------------------------------------------------------------
# 验证 GasCooldown.h 的纯算术核心（gas_cooldown::StepCooldown / IsOnCooldown /
# StartCooldown）。与 EnhancedInput 冒烟测试同一手法：被测逻辑是 header-only 的
# 纯函数（不依赖 ASC/GameplayAbility/AttributeSet/GameObject），故本 target 只
# 链接 include root，不链 ZRuntime。
#
# 冷却状态落在 GameplayAbilitySpec 上（GasTypes.h 里薄封装转发到这里的算术），
# ASC 每帧遍历调用；spec 层接线由编译期类型检查保证，故本测试聚焦算术语义。
#
#   cmake -B build -DZENGINE_BUILD_GAS_COOLDOWN_SMOKE_TEST=ON
#   cmake --build build --target GasCooldownSmokeTest
# =====================================================================

option(ZENGINE_BUILD_GAS_COOLDOWN_SMOKE_TEST
       "Build the GAS cooldown arithmetic smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_GAS_COOLDOWN_SMOKE_TEST)
    return()
endif()

add_executable(GasCooldownSmokeTest GasCooldownSmokeTest.cpp)

set_target_properties(GasCooldownSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# Header-only include chain: only the engine/Source root is needed.
target_include_directories(GasCooldownSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source
)

if(MSVC)
    target_compile_options(GasCooldownSmokeTest PRIVATE /W4)
endif()

add_test(NAME GasCooldownSmokeTest COMMAND GasCooldownSmokeTest)

message(STATUS "Configured GAS cooldown smoke-test target: GasCooldownSmokeTest")

# =====================================================================
# GAS 周期性效果冒烟测试（周期性效果执行落地验证）
# ---------------------------------------------------------------------
# 验证 GasPeriodic.h 的纯算术核心（gas_periodic::PendingExecutions /
# IsValidPeriod）。与 GasCooldownSmokeTest 同一手法：被测逻辑是 header-only
# 的纯函数（不依赖 ASC/GameplayEffect/GameplayEffectSpec/GameObject），故本
# target 只链接 include root，不链 ZRuntime。
#
# 周期计时状态落在 GameplayEffectSpec 上（GasTypes.h 里 StepPeriodic 薄封装转发
# 到这里的算术），ASC 每帧遍历按返回次数调 onPeriodicExecute；spec 层接线由编译
# 期类型检查保证，故本测试聚焦算术语义（跨周期补触发、病态输入防御、帧率无关）。
#
#   cmake -B build -DZENGINE_BUILD_GAS_PERIODIC_SMOKE_TEST=ON
#   cmake --build build --target GasPeriodicSmokeTest
# =====================================================================

option(ZENGINE_BUILD_GAS_PERIODIC_SMOKE_TEST
       "Build the GAS periodic-effect timing smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_GAS_PERIODIC_SMOKE_TEST)
    return()
endif()

add_executable(GasPeriodicSmokeTest GasPeriodicSmokeTest.cpp)

set_target_properties(GasPeriodicSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# Header-only include chain: only the engine/Source root is needed.
target_include_directories(GasPeriodicSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source
)

if(MSVC)
    target_compile_options(GasPeriodicSmokeTest PRIVATE /W4)
endif()

add_test(NAME GasPeriodicSmokeTest COMMAND GasPeriodicSmokeTest)

message(STATUS "Configured GAS periodic smoke-test target: GasPeriodicSmokeTest")
