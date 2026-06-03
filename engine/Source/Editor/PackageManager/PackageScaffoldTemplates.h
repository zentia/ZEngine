#pragma once

/// Default <Project>/Packages/manifest.json written on first project open.
inline constexpr const char* kProjectPackagesManifestTemplate = R"({
  "dependencies": {
    "com.zengine.ugui": "1.0.0"
  }
}
)";
