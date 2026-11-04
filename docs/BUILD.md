# Building NOPEAKS

NOPEAKS has a dependency-free C++17 DSP/control core and two JUCE VST3 products: **NOPEAKS Master** and **NOPEAKS Track**. Standalone apps are built for UI and audio-device checks; they are not substitutes for testing the two VST3s together inside a DAW.

The JUCE dependency is pinned to **8.0.12**, commit `29396c22c93392d6738e021b83196283d6e4d850`. CMake fetches it from the official JUCE repository. An existing checkout can be supplied with `-DNOPEAKS_JUCE_PATH=/absolute/path/to/JUCE`. JUCE's licensing terms apply independently of this project's source; commercial distribution requires an appropriate JUCE licence or compliance with its open-source terms.

## Requirements

- CMake 3.22 or later; a C++17 compiler.
- Windows: Visual Studio 2022 with Desktop development with C++.
- macOS: Xcode command-line tools and macOS 11 or later deployment target.
- Linux: compiler, ALSA, X11/Xext/Xinerama/Xrandr/Xcursor, FreeType, fontconfig and OpenGL development headers. On Ubuntu: `sudo apt-get install build-essential cmake ninja-build pkg-config libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`.

## Core tests without downloading JUCE

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Debug -DNOPEAKS_BUILD_PLUGINS=OFF
cmake --build build/core --config Debug --parallel 2
ctest --test-dir build/core -C Debug --output-on-failure
```

On GCC/Clang, add `-DNOPEAKS_SANITIZE=ON` for address and undefined-behaviour checks. `cmake --preset core`, `cmake --build --preset core` and `ctest --preset core` are equivalent when Ninja is available. The suite includes coordinated audio and audio quality regressions in addition to the individual DSP/transport tests. Restricted execution environments may prevent LeakSanitizer from inspecting `/proc`; locally, `ASAN_OPTIONS=detect_leaks=0` can keep address/undefined-behaviour checks running in that situation. CI retains its normal leak checks.

## VST3 builds

Windows, from a Developer PowerShell:

```powershell
cmake -S . -B build/plugins -A x64
cmake --build build/plugins --config Release --parallel 2
ctest --test-dir build/plugins -C Release --output-on-failure
python scripts/package.py --build-dir build/plugins --output dist/NOPEAKS-Windows-x64.zip
```

macOS universal build:

```sh
cmake -S . -B build/plugins -DCMAKE_BUILD_TYPE=Release '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build/plugins --config Release --parallel 2
ctest --test-dir build/plugins -C Release --output-on-failure
python3 scripts/package.py --build-dir build/plugins --output dist/NOPEAKS-macOS-universal.zip
```

Linux:

```sh
cmake -S . -B build/plugins -DCMAKE_BUILD_TYPE=Release
cmake --build build/plugins --parallel 2
ctest --test-dir build/plugins --output-on-failure
python3 scripts/package.py --build-dir build/plugins --output dist/NOPEAKS-Linux-x64.zip
```

Products are under `build/plugins/NOPEAKS_Master_artefacts/[Release/]VST3/` and the corresponding `NOPEAKS_Track_artefacts` directory. Install both complete `.vst3` bundles, preserving their directory structures.

With plug-ins and tests enabled, `nopeaks_host_tests` links the compiled processor/editor code and exercises both roles through their explicit constructors. CTest runs it automatically; on Linux, CMake uses `xvfb-run` if available for GUI initialization. Use `-DNOPEAKS_BUILD_HOST_TESTS=OFF` only when building in an environment that cannot run these checks. This harness tests processor integration and does not replace loading the actual VST3 binaries in a DAW.

| Platform | User or system VST3 installation directory |
| --- | --- |
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` |
| Linux | `~/.vst3/` |

## GitHub Actions artifacts

The **Build and test NOPEAKS** workflow runs core tests with sanitizers, then independently builds Windows x64, macOS universal and Linux x64 plug-ins. Each successful platform job uploads a ZIP containing both VST3 products. Open the successful Actions run and download the platform artifact. Artifacts are retained for 14 days; a new workflow run can rebuild the pinned source.

CI packages are unsigned. This repository does not include Apple Developer credentials, notarization, a Windows signing certificate or a PACE/AAX build. Successful compilation and synthetic tests do not establish DAW interoperability or perceptual quality. Before release, validate the VST3s with Steinberg's validator or pluginval and test real-time playback, offline export, host bypass, sample-rate changes, seeks, loop boundaries, session restore, crash recovery, independent plug-in processes and the documented routing constraints in the target DAWs. Use a signing/notarization pipeline before public macOS distribution.
