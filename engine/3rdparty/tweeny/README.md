# Tweeny Integration

[Tweeny](https://github.com/mobius3/tweeny) is a modern C++ tweening library for smooth animations.

## Installation

### Automatic Installation (Windows)

Run the PowerShell script from the `engine/3rdparty` directory:

```powershell
.\download_tweeny.ps1
```

### Manual Installation

1. Download tweeny from GitHub: https://github.com/mobius3/tweeny/releases
2. Extract the archive
3. Copy the `include` directory to `engine/3rdparty/tweeny/include`
4. The final structure should be:
   ```
   engine/3rdparty/tweeny/include/tweeny.h
   ```

## Usage

After installation, tweeny is automatically available in the ZEngine project. Include it in your code:

```cpp
#include <tweeny.h>

// Example: Tween a float value
auto tween = tweeny::from(0.0f).to(100.0f).during(1000).via(tweeny::easing::elasticOut);

// Update in your game loop
float delta_time = 0.016f; // 60fps
tween.step(static_cast<int>(delta_time * 1000)); // step expects milliseconds
float current_value = tween.peek();

// Or use the progress-based API
tween.progress(0.5f); // Set to 50% progress
float value = tween.peek();
```

## Features

- Header-only library (no linking required)
- Modern C++17 API
- Chainable API similar to DOTween
- Multiple easing functions
- Supports various value types (float, int, vectors, etc.)

## Documentation

For more information, visit: https://github.com/mobius3/tweeny

