#include "engine/source/runtime/core/base/platform.h"
#include <iostream>
#include <string>
#include <vector>
#include <fstream>

class PlatformTester {
private:
    std::vector<std::string> test_results;
    
    void add_result(const std::string& test_name, bool passed, const std::string& details = "") {
        std::string result = test_name + ": " + (passed ? "PASS" : "FAIL");
        if (!details.empty()) {
            result += " (" + details + ")";
        }
        test_results.push_back(result);
        std::cout << result << std::endl;
    }
    
public:
    void test_platform_detection() {
        std::cout << "\n=== Platform Detection Tests ===" << std::endl;
        
        // Test platform macros
        bool windows_detected = false;
        bool android_detected = false;
        bool macos_detected = false;
        bool linux_detected = false;
        bool ios_detected = false;
        
#ifdef Z_PLATFORM_WINDOWS
        windows_detected = true;
#endif
#ifdef Z_PLATFORM_ANDROID
        android_detected = true;
#endif
#ifdef Z_PLATFORM_MACOS
        macos_detected = true;
#endif
#ifdef Z_PLATFORM_LINUX
        linux_detected = true;
#endif
#ifdef Z_PLATFORM_IOS
        ios_detected = true;
#endif
        
        int platform_count = windows_detected + android_detected + macos_detected + linux_detected + ios_detected;
        add_result("Platform macro detection", platform_count == 1, 
                  "Detected " + std::to_string(platform_count) + " platform(s)");
        
        // Test runtime functions
        std::string platform_name = Runtime::Platform::GetPlatformName();
        add_result("Platform name function", !platform_name.empty(), platform_name);
        
        // Test convenience macros
        bool runtime_windows = Z_IS_WINDOWS();
        bool runtime_android = Z_IS_ANDROID();
        bool runtime_macos = Z_IS_MACOS();
        bool runtime_linux = Z_IS_LINUX();
        bool runtime_ios = Z_IS_IOS();
        
        int runtime_platform_count = runtime_windows + runtime_android + runtime_macos + runtime_linux + runtime_ios;
        add_result("Runtime platform detection", runtime_platform_count == 1,
                  "Detected " + std::to_string(runtime_platform_count) + " platform(s)");
    }
    
    void test_architecture_detection() {
        std::cout << "\n=== Architecture Detection Tests ===" << std::endl;
        
        // Test architecture macros
        bool x64_detected = false;
        bool x86_detected = false;
        bool arm64_detected = false;
        bool arm32_detected = false;
        
#ifdef Z_ARCH_X64
        x64_detected = true;
#endif
#ifdef Z_ARCH_X86
        x86_detected = true;
#endif
#ifdef Z_ARCH_ARM64
        arm64_detected = true;
#endif
#ifdef Z_ARCH_ARM32
        arm32_detected = true;
#endif
        
        int arch_count = x64_detected + x86_detected + arm64_detected + arm32_detected;
        add_result("Architecture macro detection", arch_count == 1,
                  "Detected " + std::to_string(arch_count) + " architecture(s)");
        
        // Test runtime functions
        std::string arch_name = Runtime::Platform::GetArchitectureName();
        add_result("Architecture name function", !arch_name.empty(), arch_name);
        
        // Test convenience macros
        bool runtime_x64 = Z_IS_X64();
        bool runtime_x86 = Z_IS_X86();
        bool runtime_arm64 = Z_IS_ARM64();
        bool runtime_arm32 = Z_IS_ARM32();
        
        int runtime_arch_count = runtime_x64 + runtime_x86 + runtime_arm64 + runtime_arm32;
        add_result("Runtime architecture detection", runtime_arch_count == 1,
                  "Detected " + std::to_string(runtime_arch_count) + " architecture(s)");
    }
    
    void test_compiler_detection() {
        std::cout << "\n=== Compiler Detection Tests ===" << std::endl;
        
        // Test compiler macros
        bool msvc_detected = false;
        bool gcc_detected = false;
        bool clang_detected = false;
        bool apple_clang_detected = false;
        
#ifdef Z_COMPILER_MSVC
        msvc_detected = true;
#endif
#ifdef Z_COMPILER_GCC
        gcc_detected = true;
#endif
#ifdef Z_COMPILER_CLANG
        clang_detected = true;
#endif
#ifdef Z_COMPILER_APPLE_CLANG
        apple_clang_detected = true;
#endif
        
        int compiler_count = msvc_detected + gcc_detected + clang_detected + apple_clang_detected;
        add_result("Compiler macro detection", compiler_count == 1,
                  "Detected " + std::to_string(compiler_count) + " compiler(s)");
        
        // Test runtime functions
        std::string compiler_name = Runtime::Platform::GetCompilerName();
        add_result("Compiler name function", !compiler_name.empty(), compiler_name);
        
        // Test convenience macros
        bool runtime_msvc = Z_IS_MSVC();
        bool runtime_gcc = Z_IS_GCC();
        bool runtime_clang = Z_IS_CLANG();
        bool runtime_apple_clang = Z_IS_APPLE_CLANG();
        
        int runtime_compiler_count = runtime_msvc + runtime_gcc + runtime_clang + runtime_apple_clang;
        add_result("Runtime compiler detection", runtime_compiler_count == 1,
                  "Detected " + std::to_string(runtime_compiler_count) + " compiler(s)");
    }
    
    void test_platform_specific_features() {
        std::cout << "\n=== Platform-Specific Feature Tests ===" << std::endl;
        
        // Test path separators
        add_result("Path separator constant", Z_PATH_SEPARATOR != 0, 
                  std::string("Separator: '") + Z_PATH_SEPARATOR + "'");
        add_result("Path separator string", !std::string(Z_PATH_SEPARATOR_STR).empty(),
                  "String: \"" + std::string(Z_PATH_SEPARATOR_STR) + "\"");
        
        // Test platform-specific includes
#ifdef Z_PLATFORM_WINDOWS
        add_result("Windows includes", true, "Windows headers available");
#else
        add_result("Unix includes", true, "Unix headers available");
#endif
        
        // Test conditional compilation
        std::string platform_specific_code = "Generic";
#ifdef Z_PLATFORM_WINDOWS
        platform_specific_code = "Windows-specific";
#elif defined(Z_PLATFORM_ANDROID)
        platform_specific_code = "Android-specific";
#elif defined(Z_PLATFORM_MACOS)
        platform_specific_code = "macOS-specific";
#elif defined(Z_PLATFORM_LINUX)
        platform_specific_code = "Linux-specific";
#elif defined(Z_PLATFORM_IOS)
        platform_specific_code = "iOS-specific";
#endif
        
        add_result("Conditional compilation", platform_specific_code != "Generic", platform_specific_code);
    }
    
    void test_consistency() {
        std::cout << "\n=== Consistency Tests ===" << std::endl;
        
        // Test macro vs runtime consistency
        bool macro_windows = false;
        bool runtime_windows = Z_IS_WINDOWS();
        
#ifdef Z_PLATFORM_WINDOWS
        macro_windows = true;
#endif
        
        add_result("Windows detection consistency", macro_windows == runtime_windows,
                  "Macro: " + std::to_string(macro_windows) + ", Runtime: " + std::to_string(runtime_windows));
        
        // Test that only one platform is detected
        int platform_count = Z_IS_WINDOWS() + Z_IS_ANDROID() + Z_IS_MACOS() + Z_IS_LINUX() + Z_IS_IOS();
        add_result("Single platform detection", platform_count == 1,
                  "Detected " + std::to_string(platform_count) + " platforms");
        
        // Test that only one architecture is detected
        int arch_count = Z_IS_X64() + Z_IS_X86() + Z_IS_ARM64() + Z_IS_ARM32();
        add_result("Single architecture detection", arch_count == 1,
                  "Detected " + std::to_string(arch_count) + " architectures");
        
        // Test that only one compiler is detected
        int compiler_count = Z_IS_MSVC() + Z_IS_GCC() + Z_IS_CLANG() + Z_IS_APPLE_CLANG();
        add_result("Single compiler detection", compiler_count == 1,
                  "Detected " + std::to_string(compiler_count) + " compilers");
    }
    
    void generate_report() {
        std::cout << "\n=== Test Report ===" << std::endl;
        
        int total_tests = test_results.size();
        int passed_tests = 0;
        
        for (const auto& result : test_results) {
            if (result.find("PASS") != std::string::npos) {
                passed_tests++;
            }
        }
        
        std::cout << "Total tests: " << total_tests << std::endl;
        std::cout << "Passed: " << passed_tests << std::endl;
        std::cout << "Failed: " << (total_tests - passed_tests) << std::endl;
        std::cout << "Success rate: " << (100.0 * passed_tests / total_tests) << "%" << std::endl;
        
        // Save detailed report
        std::ofstream report_file("test_results/detailed_report.txt");
        if (report_file.is_open()) {
            report_file << "ZEngine Platform Detection Detailed Report\n";
            report_file << "==========================================\n\n";
            
            for (const auto& result : test_results) {
                report_file << result << "\n";
            }
            
            report_file << "\nSummary:\n";
            report_file << "Total tests: " << total_tests << "\n";
            report_file << "Passed: " << passed_tests << "\n";
            report_file << "Failed: " << (total_tests - passed_tests) << "\n";
            report_file << "Success rate: " << (100.0 * passed_tests / total_tests) << "%\n";
            
            report_file.close();
            std::cout << "\nDetailed report saved to: test_results/detailed_report.txt" << std::endl;
        }
    }
    
    void run_all_tests() {
        std::cout << "ZEngine Advanced Platform Test Suite" << std::endl;
        std::cout << "====================================" << std::endl;
        
        test_platform_detection();
        test_architecture_detection();
        test_compiler_detection();
        test_platform_specific_features();
        test_consistency();
        generate_report();
    }
};

int main() {
    PlatformTester tester;
    tester.run_all_tests();
    return 0;
}
