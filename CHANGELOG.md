# Changelog

All notable changes to ZEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added - 2026-02-11
- **Memory Profiler Enhancements**
  - Added C# memory profiling support (`CSharpMemoryProfiler`)
  - Enhanced Lua memory profiling with better tracking and statistics
  - Improved UI for memory profiler with detailed memory breakdown
  - Added memory tracking utilities in `Utility.h`
  - New memory profiler APIs in ZEngineApi for external integration
  - Added EASTL visualization support (EASTL.natvis)

- **Console System**
  - New ConsoleWindow component for Editor (`engine/Source/Editor/editor_window/ConsoleWindow/`)
  - Integrated console manager functionality

- **File System**
  - Extended FileSystem API with new file operations
  - Windows-specific file system enhancements in `LocalFileSystemWindowsShared`

- **Scripting**
  - Lua loader improvements with better module management
  - Puerts integration enhancements for JavaScript/TypeScript support
  - Native scripting API additions (`PuertsNative.h`)
  - ScriptObject and ObjectPool for better script object lifecycle management
  - New BackendLua implementation for Lua scripting backend

- **Build System**
  - Added CommonPCH (precompiled headers) for faster compilation
  - New `generate_engine_log.cmake` for automated log generation
  - Improved CMake configuration for output directories
  - Better dependency management for 3rd party libraries

- **Documentation**
  - Added `LINK_ORDER_MEMORY_MANAGER.md` documenting memory manager linking strategies

### Changed - 2026-02-11
- **Profiler Refactor**
  - Reorganized profiler architecture with better separation of concerns
  - Updated ProfilerRuntime with new profiling hooks
  - Enhanced MemoryProfiler base class for extensibility

- **Resource Management**
  - AssetManager API improvements
  - ConfigManager refactoring for better configuration handling
  - AssetBundle optimizations

- **Serialization System**
  - Enhanced TypeTree cache system
  - Improved serialize traits for better type handling
  - JSON serialization improvements

- **Editor Updates**
  - EditorApplication initialization improvements
  - Better resource management in editor
  - Improved project window file handling

- **Code Quality**
  - Removed deprecated container implementations (HashSet, HashMap alternatives)
  - Cleaned up unused includes and dependencies
  - Better EASTL integration
  - Reduced PCH overhead by moving to CommonPCH

### Fixed
- Memory manager linking order issues (see `doc/LINK_ORDER_MEMORY_MANAGER.md`)
- VirtualFileSystem path handling
- Encoding utilities edge cases

## [Previous Changes] - 2026-01-01 to 2026-02-10

### Added
- **Serialization System** (Multiple commits Jan 1-30)
  - Complete binary serialization/deserialization framework
  - TypeTree-based reflection system
  - JSON serialization support
  - Type registration and metadata system

- **Resource Management** (Jan 2, 21)
  - ResourceManager implementation
  - AssetManager for asset loading and caching
  - Asset bundle support

- **Profiling Infrastructure** (Jan 10, 18)
  - Memory profiling foundation
  - Performance profiling utilities
  - Memory analysis tools

- **File Operations** (Jan 14, 17)
  - Binary file read/write support
  - File operation utilities
  - Virtual file system improvements

- **Third-party Libraries** (Jan 2)
  - Integrated xxHash for fast hashing

### Changed
- Repository reinitialized with clean history (Jan 22)

---

## Version History Notes

### Migration from Previous Repository
On 2026-01-22, the repository was reinitialized with a clean history. Previous history before this date has been archived.

### Commit Conventions
- Commits in Chinese indicate internal development work
- Major features are documented in this CHANGELOG
- See git history for detailed commit messages

---

## Contributing
When making changes:
1. Update this CHANGELOG under the [Unreleased] section
2. Follow the format: Added/Changed/Deprecated/Removed/Fixed/Security
3. Include relevant file paths or component names
4. Reference issue numbers where applicable

