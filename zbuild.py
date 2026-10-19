#!/usr/bin/env python3
"""
ZEngine Build System
A unified build tool layer for ZEngine that provides a consistent interface
across all platforms, similar to Unreal's UAT but lighter weight.
"""

import argparse
import subprocess
import sys
import os
import platform
import shutil
from pathlib import Path
from typing import Any, Optional, List, Dict
import json


class Colors:
    """ANSI color codes for terminal output"""
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'


def print_info(message: str):
    print(f"{Colors.OKCYAN}[INFO]{Colors.ENDC} {message}")


def print_success(message: str):
    print(f"{Colors.OKGREEN}[SUCCESS]{Colors.ENDC} {message}")


def print_warning(message: str):
    print(f"{Colors.WARNING}[WARNING]{Colors.ENDC} {message}")


def print_error(message: str):
    print(f"{Colors.FAIL}[ERROR]{Colors.ENDC} {message}")


def print_header(message: str):
    print(f"\n{Colors.BOLD}{Colors.HEADER}{'='*60}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.HEADER}{message}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.HEADER}{'='*60}{Colors.ENDC}\n")


# CMake configuration name for each zbuild.py --config value. The engine adds
# DebugV8 on top of the four stock ones (install already accepted it).
CONFIG_MAP = {
    "debug": "Debug",
    "release": "Release",
    "relwithdebinfo": "RelWithDebInfo",
    "debugv8": "DebugV8",
}

# The four standalone GUI tools and *how* each one has to be built. They share a
# source directory (engine/tools/) and an output root (bin/Tools/<Config>/, set
# by z_set_tool_output_dir in engine/cmake/ToolOutputDir.cmake), but not a build
# tree, because they do not share dependencies:
#
#   intree     -- links ZRuntime, so it can only come out of a full-tree
#                 configure. Slower the first time, incremental afterwards.
#                 ZEnvironmentCollector is intree for the same reason as the
#                 other two plus one more: it links ZRuntimeShared for its log
#                 and its localization DataTable, and a standalone configure of
#                 engine/tools/WindowsEnvironmentCheck has no ZRuntimeShared
#                 target (producing one would mean pulling in all of
#                 engine/Source/Runtime). "tree_options" is what turns the whole
#                 tree /MT so the shipped directory still needs no VC redist;
#                 it only takes effect on a FRESH configure (pass
#                 --reconfigure otherwise).
#   standalone -- self-contained project with its own configure. Kept out of the
#                 full tree on purpose: ClientProjBranches is OFF there. The
#                 collector's own configure still works (it falls back to a
#                 statically linked bqlog and a compiled-in string table), which
#                 is why its CMake still carries the ENVIRONMENT_CHECK_STANDALONE
#                 branch -- but that is no longer what ships.
#
# "subdir" is the optional extra level a tool asks for via
# z_set_tool_output_dir(... SUBDIR <name>). WindowsEnvironmentCheck uses it
# because it ships an .exe plus a helper exe, profiles/ and LICENSE files --
# dropped straight into bin/Tools/<Config>/ those cannot be told apart from
# another tool's. Must match the CMake side, hence the comment there too.
TOOLS: Dict[str, Dict[str, Any]] = {
    "TexPreview": {
        "mode": "intree",
        "source": "engine/tools/TexPreview",
        "exe": "TexPreview.exe",
    },
    "LuaMemPreview": {
        "mode": "intree",
        "source": "engine/tools/LuaMemPreview",
        "exe": "LuaMemPreview.exe",
    },
    "ClientProjBranches": {
        "mode": "standalone",
        "source": "engine/tools/ClientProjBranches",
        "build_dir": "build/tools/ClientProjBranches",
        "targets": ["ClientProjBranches"],
        "exe": "ClientProjBranches.exe",
    },
    "ZEnvironmentCollector": {
        "mode": "intree",
        "source": "engine/tools/WindowsEnvironmentCheck",
        # What ships: engine-backed (LogSystem + DataTable localization) and
        # whole-tree /MT so the output directory needs no VC redist.
        "tree_options": ["-DZENGINE_STATIC_CRT=ON",
                         "-DENVIRONMENT_CHECK_BUILD_GUI=ON"],
        "targets": ["ZEnvironmentCollector", "ZDllLoadProbe"],
        "subdir": "WindowsEnvironmentCheck",
        "exe": "ZEnvironmentCollector.exe",
    },
}


class BuildSystem:
    """Main build system class"""

    @staticmethod
    def _cmake_subprocess_env() -> Dict[str, str]:
        """CMake preset windows_visual_studio requires VSCODE_NLS_CONFIG empty."""
        env = os.environ.copy()
        if env.get("VSCODE_NLS_CONFIG"):
            print_info(
                "Cursor/VSCode detected: clearing VSCODE_NLS_CONFIG so "
                "windows_visual_studio preset is available"
            )
            env.pop("VSCODE_NLS_CONFIG", None)
        return env

    def __init__(self, root_dir: Optional[Path] = None):
        self.root_dir = root_dir or Path(__file__).parent.absolute()
        self.build_dir = self.root_dir / "build"
        self.system = platform.system()
        self.is_windows = self.system == "Windows"
        self.is_linux = self.system == "Linux"
        self.is_macos = self.system == "Darwin"
        
    def check_requirements(self) -> bool:
        """Check if required tools are available"""
        print_info("Checking build requirements...")
        
        # Check CMake
        cmake_version = self._check_command("cmake", "--version")
        if not cmake_version:
            print_error("CMake is not installed or not in PATH")
            return False
        
        print_info(f"Found CMake: {cmake_version.split()[2]}")
        
        # Check compiler
        if self.is_windows:
            # Check for Visual Studio
            vs_where = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
            if vs_where.exists():
                print_info("Found Visual Studio")
            else:
                print_warning("Visual Studio may not be installed")
        elif self.is_linux or self.is_macos:
            clang = self._check_command("clang++", "--version")
            if clang:
                print_info(f"Found Clang: {clang.split()[2]}")
            else:
                print_warning("Clang++ not found, falling back to default compiler")
        
        # Check for ccache (optional)
        ccache = self._check_command("ccache", "--version")
        if ccache:
            print_info(f"Found ccache: {ccache.split()[2]}")
        else:
            print_info("ccache not found (optional, but recommended for faster builds)")
        
        return True
    
    def _check_command(self, cmd: str, arg: str = "--version") -> Optional[str]:
        """Check if a command exists and return its version"""
        try:
            result = subprocess.run(
                [cmd, arg],
                capture_output=True,
                text=True,
                timeout=5
            )
            if result.returncode == 0:
                return result.stdout.strip().split('\n')[0]
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
        return None
    
    def get_preset_name(self, config: str, generator: Optional[str] = None) -> str:
        """Get CMake preset name based on platform and configuration"""
        if self.is_windows:
            if generator == "ninja":
                preset = "windows_ninja"
            else:
                preset = "windows_visual_studio"
            return preset
        elif self.is_linux:
            if generator == "ninja":
                preset = "linux_ninja"
            else:
                preset = "linux_make"
            return f"{preset}_{config}"
        elif self.is_macos:
            if generator == "ninja":
                preset = "macos_ninja"
            else:
                preset = "macos_xcode"
            return f"{preset}_{config}"
        else:
            raise RuntimeError(f"Unsupported platform: {self.system}")
    
    def configure(self, config: str = "debug", generator: Optional[str] = None, 
                  preset: Optional[str] = None, extra_args: Optional[List[str]] = None) -> bool:
        """Configure the build system"""
        print_header(f"Configuring ZEngine ({config})")
        
        # Use preset if available
        if preset:
            print_info(f"Using preset: {preset}")
            cmd = ["cmake", "--preset", preset]
        else:
            # Fallback to manual configuration
            preset_name = self.get_preset_name(config, generator)
            print_info(f"Using preset: {preset_name}")
            cmd = ["cmake", "--preset", preset_name]
        
        if extra_args:
            cmd.extend(extra_args)
        
        print_info(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=self.root_dir, env=self._cmake_subprocess_env())

        if result.returncode != 0:
            print_error("Configuration failed")
            return False

        print_success("Configuration completed successfully")
        return True

    def build(self, config: str = "debug", target: Optional[str] = None,
              jobs: Optional[int] = None, preset: Optional[str] = None) -> bool:
        """Build the project"""
        print_header(f"Building ZEngine ({config})")
        
        # Determine build directory
        build_dir = self.build_dir
        
        # Build command
        if preset:
            cmd = ["cmake", "--build", "--preset", preset]
        else:
            cmd = ["cmake", "--build", str(build_dir)]
            
            # Configuration
            if self.is_windows:
                cmd.extend(["--config", self._cmake_config_name(config)])
            else:
                # For Unix-like systems, use CMAKE_BUILD_TYPE
                pass
        
        # Target
        if target:
            cmd.extend(["--target", target])
        
        # Jobs - Enable parallel builds for all platforms
        if jobs:
            cmd.extend(["-j", str(jobs)])
        else:
            # Auto-detect CPU count for all platforms
            try:
                import multiprocessing
                cpu_count = multiprocessing.cpu_count()
                # Use all available cores for faster builds
                cmd.extend(["-j", str(cpu_count)])
                print_info(f"Using {cpu_count} parallel jobs (auto-detected)")
            except:
                pass
        
        print_info(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=self.root_dir, env=self._cmake_subprocess_env())

        if result.returncode != 0:
            print_error("Build failed")
            return False
        
        print_success("Build completed successfully")
        return True
    
    @staticmethod
    def _cmake_config_name(config: str) -> str:
        """zbuild.py --config value -> CMake configuration name."""
        return CONFIG_MAP.get(config.lower(), config)

    def list_tools(self):
        """List the standalone GUI tools zbuild.py knows how to build."""
        print_header("Standalone GUI tools")
        for name, spec in TOOLS.items():
            if spec["mode"] == "intree":
                how = "full tree  (links ZRuntime, needs zbuild.py configure once)"
            else:
                how = "own configure  (build/tools/" + name + ")"
            print(f"  {name:<22} {how}")
        print("\n  Build one:  zbuild.py tool <Name> --config release")
        print("  Build all:  zbuild.py tool --all")
        print("  Output:     bin/Tools/<Config>/, or")
        print("              bin/Tools/<Config>/<subdir>/ for a tool that ships")
        print("              more than its .exe.")

    def build_all_tools(self, config: str = "release", jobs: Optional[int] = None,
                        generator: Optional[str] = None,
                        reconfigure: bool = False) -> bool:
        """Build every tool in TOOLS. Stops at the first failure."""
        print_header(f"Building all {len(TOOLS)} GUI tools "
                     f"({self._cmake_config_name(config)})")
        print_info("TexPreview / LuaMemPreview come from the full tree -- the "
                   "first run also configures it, which is the slow part")
        built = []
        for name in TOOLS:
            if not self.build_tool(name, config=config, jobs=jobs,
                                   generator=generator, reconfigure=reconfigure):
                print_error(f"Stopped after {len(built)}/{len(TOOLS)} tools "
                            f"(built: {', '.join(built) or 'none'})")
                return False
            built.append(name)
        print_success(f"Built {len(built)}/{len(TOOLS)} tools: {', '.join(built)}")
        return True

    def build_tool(self, name: str, config: str = "release",
                   jobs: Optional[int] = None, generator: Optional[str] = None,
                   reconfigure: bool = False) -> bool:
        """Build one of the standalone GUI tools registered in TOOLS."""
        spec = TOOLS.get(name)
        if spec is None:
            print_error(f"Unknown tool '{name}'. Known tools: {', '.join(TOOLS)}")
            return False

        cmake_config = self._cmake_config_name(config)
        print_header(f"Building tool {name} ({cmake_config})")

        if spec["mode"] == "intree":
            tree_options = spec.get("tree_options", [])
            if reconfigure or not (self.build_dir / "CMakeCache.txt").exists():
                print_info("Full tree is not configured yet -- configuring it first")
                if not self.configure(config=config, generator=generator,
                                      extra_args=tree_options):
                    return False
            elif tree_options:
                # Cached -D values win over command-line ones on a reconfigure,
                # so a build tree that predates the option silently ignores it.
                print_info(f"{name} wants {tree_options}, but the tree is already "
                           "configured -- those only apply to a fresh one. Pass "
                           "--reconfigure if the current tree was not configured "
                           "with them.")
            # Explicit target list so helper executables come along too (the
            # collector needs ZDllLoadProbe beside it). Falls back to the tool
            # name when a spec declares none.
            cmd = ["cmake", "--build", str(self.build_dir), "--config", cmake_config]
            for target in spec.get("targets", [name]):
                cmd.extend(["--target", target])
        else:
            build_dir = self.root_dir / spec["build_dir"]
            if reconfigure or not (build_dir / "CMakeCache.txt").exists():
                cfg_cmd = ["cmake", "-S", str(self.root_dir / spec["source"]),
                           "-B", str(build_dir)]
                if self.is_windows:
                    cfg_cmd.extend(["-G", "Visual Studio 17 2022", "-A", "x64"])
                cfg_cmd.extend(spec.get("options", []))
                print_info(f"Running: {' '.join(cfg_cmd)}")
                configured = subprocess.run(
                    cfg_cmd, cwd=self.root_dir, env=self._cmake_subprocess_env())
                if configured.returncode != 0:
                    print_error(f"Configuration of {name} failed")
                    return False
            cmd = ["cmake", "--build", str(build_dir), "--config", cmake_config]
            for target in spec.get("targets", []):
                cmd.extend(["--target", target])

        if jobs:
            cmd.extend(["-j", str(jobs)])
        else:
            try:
                import multiprocessing
                cmd.extend(["-j", str(multiprocessing.cpu_count())])
            except Exception:
                pass

        print_info(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=self.root_dir, env=self._cmake_subprocess_env())
        if result.returncode != 0:
            print_error(f"Build of {name} failed")
            return False

        # z_set_tool_output_dir sends every tool to
        # bin/Tools/<Config>/[<subdir>/], but -DZENGINE_OUTPUT_SUBDIR moves the
        # whole bin/ layout, so look it up instead of asserting one path.
        subdir = spec.get("subdir")
        leaf = f"{subdir}/{spec['exe']}" if subdir else spec["exe"]
        exe = self.root_dir / "bin" / "Tools" / cmake_config / leaf
        if not exe.exists():
            found = sorted(
                self.root_dir.glob(f"bin/**/Tools/{cmake_config}/{leaf}"))
            if found:
                exe = found[0]
        if exe.exists():
            print_success(f"{name} -> {exe}")
        else:
            print_warning(f"{name} built, but {exe} was not found -- "
                          "check ZENGINE_OUTPUT_SUBDIR and the build log")
        return True

    def clean(self) -> bool:
        """Clean build artifacts"""
        print_header("Cleaning build directory")
        
        if self.build_dir.exists():
            print_info(f"Removing: {self.build_dir}")
            try:
                shutil.rmtree(self.build_dir)
                print_success("Build directory cleaned")
                return True
            except Exception as e:
                print_error(f"Failed to clean: {e}")
                return False
        else:
            print_info("Build directory does not exist, nothing to clean")
            return True
    
    def install(self, config: str = "release") -> bool:
        """Install the built artifacts"""
        print_header("Installing ZEngine")
        
        cmd = ["cmake", "--install", str(self.build_dir)]
        
        if self.is_windows:
            cmd.extend(["--config", self._cmake_config_name(config)])
        
        print_info(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=self.root_dir)
        
        if result.returncode != 0:
            print_error("Installation failed")
            return False
        
        print_success("Installation completed successfully")
        return True
    
    def test(self) -> bool:
        """Run tests"""
        print_header("Running tests")
        
        cmd = ["ctest", "--test-dir", str(self.build_dir), "--output-on-failure"]
        
        print_info(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=self.root_dir)
        
        if result.returncode != 0:
            print_error("Tests failed")
            return False
        
        print_success("All tests passed")
        return True
    
    def list_presets(self):
        """List available CMake presets"""
        print_header("Available CMake Presets")
        
        cmd = ["cmake", "--list-presets"]
        result = subprocess.run(cmd, cwd=self.root_dir, capture_output=True, text=True)
        
        if result.returncode == 0:
            print(result.stdout)
        else:
            print_error("Failed to list presets")
    
    def show_info(self):
        """Show build system information"""
        print_header("ZEngine Build System Information")
        print_info(f"Root directory: {self.root_dir}")
        print_info(f"Build directory: {self.build_dir}")
        print_info(f"Platform: {self.system}")
        print_info(f"Python: {sys.version}")
        
        # Check CMake version
        cmake_version = self._check_command("cmake", "--version")
        if cmake_version:
            print_info(f"CMake: {cmake_version}")


def main():
    parser = argparse.ArgumentParser(
        description="ZEngine Build System - Unified build tool for all platforms",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  zbuild.py configure                  # Configure with default settings
  zbuild.py configure --config release # Configure for release
  zbuild.py build                      # Build the project
  zbuild.py build --target ZEditor     # Build specific target
  zbuild.py build --jobs 8             # Build with 8 parallel jobs
  zbuild.py tools                      # List the standalone GUI tools
  zbuild.py tool TexPreview            # Build one -> bin/Tools/Release/
  zbuild.py tool --all                 # Build all four
  zbuild.py clean                      # Clean build directory
  zbuild.py test                       # Run tests
  zbuild.py install                    # Install built artifacts
        """
    )
    
    subparsers = parser.add_subparsers(dest='command', help='Command to execute')
    
    # Configure command
    configure_parser = subparsers.add_parser('configure', help='Configure the build system')
    configure_parser.add_argument('--config', choices=['debug', 'release', 'relwithdebinfo'],
                                 default='debug', help='Build configuration (debug = QuickJS, release/relwithdebinfo = V8)')
    configure_parser.add_argument('--generator', choices=['ninja', 'make', 'xcode', 'vs'],
                                 help='CMake generator to use')
    configure_parser.add_argument('--preset', help='Use specific CMake preset')
    configure_parser.add_argument('--extra-args', nargs=argparse.REMAINDER, help='Extra CMake arguments')
    
    # Build command
    build_parser = subparsers.add_parser('build', help='Build the project')
    build_parser.add_argument('--config', choices=['debug', 'release', 'relwithdebinfo'],
                             default='debug', help='Build configuration (debug = QuickJS, release/relwithdebinfo = V8)')
    build_parser.add_argument('--target', help='Specific target to build')
    build_parser.add_argument('--jobs', type=int, help='Number of parallel jobs')
    build_parser.add_argument('--preset', help='Use specific build preset')
    
    # Tool command: build one of the standalone GUI tools
    tool_parser = subparsers.add_parser('tool', help='Build one of the standalone GUI tools')
    tool_parser.add_argument('name', nargs='?',
                             help='Tool name (see "zbuild.py tools"); omit or pass '
                                  '--all to build every tool')
    tool_parser.add_argument('--config', choices=['debug', 'release', 'relwithdebinfo'],
                             default='release', help='Build configuration (default: release)')
    tool_parser.add_argument('--jobs', type=int, help='Number of parallel jobs')
    tool_parser.add_argument('--generator', choices=['ninja', 'make', 'xcode', 'vs'],
                             help='CMake generator (full-tree tools only)')
    tool_parser.add_argument('--reconfigure', action='store_true',
                             help='Force a (re)configure even if a build tree already exists')
    tool_parser.add_argument('--all', action='store_true',
                             help='Build every registered tool (same as omitting the name)')

    subparsers.add_parser('tools', help='List the standalone GUI tools')

    # Clean command
    subparsers.add_parser('clean', help='Clean build artifacts')
    
    # Install command
    install_parser = subparsers.add_parser('install', help='Install built artifacts')
    install_parser.add_argument('--config', choices=['debug', 'release', 'relwithdebinfo', 'debugv8'],
                               default='release', help='Build configuration')
    
    # Test command
    subparsers.add_parser('test', help='Run tests')
    
    # Info command
    subparsers.add_parser('info', help='Show build system information')
    
    # List presets command
    subparsers.add_parser('list-presets', help='List available CMake presets')
    
    # Check command
    subparsers.add_parser('check', help='Check build requirements')
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return 1
    
    build_system = BuildSystem()
    
    try:
        if args.command == 'check':
            if not build_system.check_requirements():
                return 1
            return 0
        
        if args.command == 'info':
            build_system.show_info()
            return 0
        
        if args.command == 'list-presets':
            build_system.list_presets()
            return 0
        
        # Check requirements for other commands
        if not build_system.check_requirements():
            print_warning("Some requirements are missing, but continuing anyway...")
        
        if args.command == 'configure':
            success = build_system.configure(
                config=args.config,
                generator=args.generator,
                preset=args.preset,
                extra_args=args.extra_args
            )
            return 0 if success else 1
        
        elif args.command == 'build':
            success = build_system.build(
                config=args.config,
                target=args.target,
                jobs=args.jobs,
                preset=args.preset
            )
            return 0 if success else 1
        
        elif args.command == 'tool':
            if args.all and args.name:
                print_error("Pass either a tool name or --all, not both")
                return 1
            if args.all or not args.name:
                success = build_system.build_all_tools(
                    config=args.config,
                    jobs=args.jobs,
                    generator=args.generator,
                    reconfigure=args.reconfigure
                )
            else:
                success = build_system.build_tool(
                    name=args.name,
                    config=args.config,
                    jobs=args.jobs,
                    generator=args.generator,
                    reconfigure=args.reconfigure
                )
            return 0 if success else 1

        elif args.command == 'tools':
            build_system.list_tools()
            return 0

        elif args.command == 'clean':
            success = build_system.clean()
            return 0 if success else 1
        
        elif args.command == 'install':
            success = build_system.install(config=args.config)
            return 0 if success else 1
        
        elif args.command == 'test':
            success = build_system.test()
            return 0 if success else 1
        
    except KeyboardInterrupt:
        print_error("\nBuild interrupted by user")
        return 1
    except Exception as e:
        print_error(f"Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())

