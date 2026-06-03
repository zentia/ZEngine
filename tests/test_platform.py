#!/usr/bin/env python3
"""
ZEngine Platform Test Runner
Cross-platform Python script for testing platform detection
"""

import os
import sys
import subprocess
import platform
import shutil
from pathlib import Path

class PlatformTestRunner:
    def __init__(self):
        self.project_root = Path(__file__).parent
        self.build_dir = self.project_root / "build"
        self.test_results_dir = self.project_root / "test_results"
        self.is_windows = platform.system() == "Windows"
        
    def print_header(self, title):
        print("=" * 50)
        print(title)
        print("=" * 50)
        
    def print_step(self, step):
        print(f"\n{step}")
        print("-" * len(step))
        
    def run_command(self, command, cwd=None, shell=None):
        """Run a command and return success status"""
        try:
            if shell is None:
                shell = self.is_windows
                
            result = subprocess.run(
                command, 
                shell=shell, 
                cwd=cwd or self.project_root,
                capture_output=True, 
                text=True,
                check=True
            )
            return True, result.stdout, result.stderr
        except subprocess.CalledProcessError as e:
            return False, e.stdout, e.stderr
        except FileNotFoundError:
            return False, "", f"Command not found: {command[0] if isinstance(command, list) else command}"
    
    def check_dependencies(self):
        """Check if required tools are available"""
        self.print_step("Checking Dependencies")
        
        # Check CMake
        success, stdout, stderr = self.run_command(["cmake", "--version"])
        if not success:
            print("[ERROR] CMake is not installed or not in PATH")
            print("Please install CMake and try again")
            return False
        else:
            version_line = stdout.split('\n')[0]
            print(f"[OK] CMake found: {version_line}")
        
        # Check Python
        print(f"[OK] Python found: {sys.version}")
        
        # Check platform info
        print(f"[OK] Current platform: {platform.system()} {platform.machine()}")
        
        return True
    
    def clean_build(self):
        """Clean previous build artifacts"""
        self.print_step("Cleaning Previous Build")
        
        try:
            if self.build_dir.exists():
                shutil.rmtree(self.build_dir)
                print("[OK] Cleaned build directory")
        except Exception as e:
            print(f"[WARNING] Could not clean build directory: {e}")
        
        try:
            if self.test_results_dir.exists():
                shutil.rmtree(self.test_results_dir)
                print("[OK] Cleaned test results directory")
        except Exception as e:
            print(f"[WARNING] Could not clean test results directory: {e}")
        
        # Create directories
        self.build_dir.mkdir(exist_ok=True)
        self.test_results_dir.mkdir(exist_ok=True)
        print("[OK] Created fresh directories")
    
    def configure_cmake(self):
        """Configure CMake"""
        self.print_step("Configuring CMake")
        
        command = ["cmake", "-B", "build", "-S", ".", "-DCMAKE_BUILD_TYPE=Debug"]
        success, stdout, stderr = self.run_command(command)
        
        if not success:
            print("[ERROR] CMake configuration failed")
            print("STDOUT:", stdout)
            print("STDERR:", stderr)
            return False
        
        print("[OK] CMake configuration successful")
        return True
    
    def build_project(self):
        """Build the project"""
        self.print_step("Building Project")
        
        command = ["cmake", "--build", "build", "--config", "Debug"]
        success, stdout, stderr = self.run_command(command)
        
        if not success:
            print("[ERROR] Build failed")
            print("STDOUT:", stdout)
            print("STDERR:", stderr)
            return False
        
        print("[OK] Build successful")
        return True
    
    def run_platform_test(self):
        """Run the platform test executable"""
        self.print_step("Running Platform Test")
        
        # Determine executable path
        if self.is_windows:
            exe_path = self.build_dir / "tests" / "Debug" / "PlatformTest.exe"
        else:
            exe_path = self.build_dir / "tests" / "PlatformTest"
        
        if not exe_path.exists():
            print(f"[ERROR] Executable not found: {exe_path}")
            return False
        
        # Run the test
        success, stdout, stderr = self.run_command([str(exe_path)])
        
        # Save output
        output_file = self.test_results_dir / "platform_output.txt"
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write("STDOUT:\n")
            f.write(stdout)
            f.write("\nSTDERR:\n")
            f.write(stderr)
        
        print("[OK] Platform test completed")
        print("\nTest Output:")
        print("-" * 30)
        print(stdout)
        if stderr:
            print("\nErrors/Warnings:")
            print(stderr)
        
        return success, stdout, stderr
    
    def run_ctest(self):
        """Run CTest if available"""
        self.print_step("Running CTest")
        
        success, stdout, stderr = self.run_command(["ctest", "--output-on-failure", "--verbose"], cwd=self.build_dir)
        
        if success:
            print("[OK] CTest completed successfully")
            print(stdout)
        else:
            print("[WARNING] CTest not available or failed")
            print(stderr)
        
        return success
    
    def analyze_results(self, stdout):
        """Analyze test results"""
        self.print_step("Test Analysis")
        
        results = {
            'platform_detected': 'Platform:' in stdout,
            'architecture_detected': 'Architecture:' in stdout,
            'compiler_detected': 'Compiler:' in stdout,
            'platform_specific_code': any(platform in stdout for platform in ['Windows', 'Android', 'macOS', 'Linux', 'iOS']),
            'architecture_specific_code': any(arch in stdout for arch in ['x86', 'x64', 'ARM']),
            'compiler_specific_code': any(compiler in stdout for compiler in ['MSVC', 'GCC', 'Clang'])
        }
        
        print("Test Results:")
        for test, passed in results.items():
            status = "[PASS]" if passed else "[FAIL]"
            print(f"  {test.replace('_', ' ').title()}: {status}")
        
        return results
    
    def generate_report(self, results):
        """Generate a test report"""
        self.print_step("Generating Test Report")
        
        report_file = self.test_results_dir / "test_report.txt"
        
        with open(report_file, 'w', encoding='utf-8') as f:
            f.write("ZEngine Platform Test Report\n")
            f.write("=" * 50 + "\n\n")
            f.write(f"Test Date: {platform.system()} {platform.machine()}\n")
            f.write(f"Python Version: {sys.version}\n\n")
            
            f.write("Test Results:\n")
            f.write("-" * 20 + "\n")
            for test, passed in results.items():
                status = "PASSED" if passed else "FAILED"
                f.write(f"{test.replace('_', ' ').title()}: {status}\n")
            
            f.write(f"\nDetailed output saved to: {self.test_results_dir / 'platform_output.txt'}\n")
        
        print(f"✅ Test report saved to: {report_file}")
    
    def run_all_tests(self):
        """Run all tests"""
        self.print_header("ZEngine Platform Test Suite")
        
        try:
            # Check dependencies
            if not self.check_dependencies():
                return False
            
            # Clean and prepare
            self.clean_build()
            
            # Configure and build
            if not self.configure_cmake():
                return False
            
            if not self.build_project():
                return False
            
            # Run tests
            success, stdout, stderr = self.run_platform_test()
            if not success:
                print("❌ Platform test failed")
                return False
            
            # Run CTest
            self.run_ctest()
            
            # Analyze results
            results = self.analyze_results(stdout)
            
            # Generate report
            self.generate_report(results)
            
            # Summary
            self.print_header("Test Summary")
            passed_tests = sum(results.values())
            total_tests = len(results)
            
            print(f"Tests passed: {passed_tests}/{total_tests}")
            
            if passed_tests == total_tests:
                print("[SUCCESS] All tests passed!")
                return True
            else:
                print("[WARNING] Some tests failed. Check the report for details.")
                return False
                
        except KeyboardInterrupt:
            print("\n[ERROR] Test interrupted by user")
            return False
        except Exception as e:
            print(f"[ERROR] Unexpected error: {e}")
            return False

def main():
    """Main entry point"""
    runner = PlatformTestRunner()
    success = runner.run_all_tests()
    
    if not success:
        sys.exit(1)

if __name__ == "__main__":
    main()
