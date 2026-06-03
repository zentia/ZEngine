#pragma once

#if defined(_WIN32)
    #if defined(ZRUNTIME_SHARED_EXPORTS)
        // Building the shared library - export symbols
        #define EXPORT_RUNTIME extern "C" __declspec(dllexport)
    #elif defined(ZRUNTIME_SHARED_IMPORTS)
        // Using the DLL - import symbols (keep C linkage)
        #define EXPORT_RUNTIME extern "C" __declspec(dllimport)
    #else
        // Building static library or other platforms - no import/export needed
        #define EXPORT_RUNTIME extern "C"
    #endif
#else
    #if defined(ZRUNTIME_SHARED_EXPORTS)
        #define EXPORT_RUNTIME extern "C" __attribute__((visibility("default")))
    #else
        #define EXPORT_RUNTIME extern "C"
    #endif
#endif
