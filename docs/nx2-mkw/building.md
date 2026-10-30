# Building on Windows

The release is built with **clang-cl** (Clang with the MSVC ABI), like Eden's own Windows CI.
On the test machine it ran clearly faster than the same code built with MSVC.

## Toolchain

| Tool | Version used | How to get it |
| --- | --- | --- |
| Visual Studio 2022 Build Tools | MSVC 14.44 | `winget install Microsoft.VisualStudio.2022.BuildTools`, workload *Desktop development with C++* plus *C++ CMake tools* |
| Clang for Windows | 19.1.5 | Visual Studio Installer, components `Microsoft.VisualStudio.Component.VC.Llvm.Clang` and `...Llvm.ClangToolset` |
| CMake, Ninja | 3.31 (ships with the Build Tools) | Included |
| Python | 3.12 | `winget install Python.Python.3.12` (needed to fetch dependencies) |
| Vulkan SDK | 1.4.341.1 | `tools/windows/install-vulkan-sdk.ps1` (run as administrator) |
| Qt | 6.8.3, `win64_msvc2022_64`, module `qtcharts` | `pip install aqtinstall`, then `python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtcharts -O C:\Qt` |

Why not the bundled Qt (`YUZU_USE_BUNDLED_QT=ON`, the default with MSVC): the bundled Qt 6.11 is
built with a newer MSVC (VS 2026) and fails to link with MSVC 14.44 (`__std_rotate`,
`__std_unique_4` unresolved). Qt 6.8.3 for MSVC 2022 links fine.

## Build

From a *x64 Native Tools* prompt (or after `vcvars64.bat`), with the Vulkan SDK, Qt and Clang on
the `PATH`:

```bat
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;C:\VulkanSDK\1.4.341.1\Bin;C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.341.1"

cmake -S . -B build/clang -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=ON -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF ^
  -DYUZU_USE_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 ^
  -DYUZU_USE_BUNDLED_SDL3=ON -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_WERROR=OFF

cmake --build build/clang --target yuzu yuzu-cmd --parallel 16

windeployqt --release --no-translations --no-compiler-runtime build\clang\bin\eden.exe
```

Output: `build\clang\bin\eden.exe` (Qt frontend) and `eden-cli.exe`.

Notes:

- `--parallel`: one job per hardware thread. 16 jobs used about 2 GB of RAM with clang-cl.
- A first build takes 40-60 minutes; later builds only recompile what changed. Headers included
  everywhere (for example `src/core/hle/service/nx2_gsv.h`) trigger long rebuilds.
- Do not run the emulator while building: both reserve a lot of memory and the build can fail
  with "the paging file is too small".
- `eden.exe` cannot be replaced while it is running: the link step then fails with
  "permission denied".

## Building with MSVC instead

Same commands without the two `CMAKE_*_COMPILER` options (build directory `build/release` for
example). MSVC treats some warnings as errors in this code base; fixes for those are in
`src/shader_recompiler/frontend/ampere/lower_to_maxwell.h` (`static_cast<u32>` on boolean
comparisons) and `src/video_core/texture_cache/util.cpp` (`static_cast<u64>` instead of a braced
narrowing conversion).

## Packaging a release

The release zip contains `build\clang\bin` after `windeployqt` (exe files, Qt DLLs and plugin
folders), `LICENSE.txt` and a README. It must never contain keys, firmware, game files, a user's
config (`qt-config.ini`) or save data.
