# Third-party notices

[← README](README.md)

## WindowSmash's own source

No project-wide license has been specified for the WindowSmash source outside the separately identified third-party material below.

## libvpx

The VP9 decoder of the WebM Project's libvpx, release v1.15.2, is included as source in [`third_party/libvpx/`](third_party/libvpx/) and compiled into the game when it is built (the game's movies are VP9). [`third_party/README.md`](third_party/README.md) says exactly which files and how they were taken from the upstream release; none is modified. The upstream license and additional patent grant are kept there:

- [BSD license and copyright notice](third_party/libvpx/LICENSE)
- [Additional IP rights grant](third_party/libvpx/PATENTS)

[`runtime/gpu_vp9_tables.h`](runtime/gpu_vp9_tables.h) also contains default VP9 probability tables derived from libvpx, as its header states.

## libopus

libopus 1.5.2 (Xiph.Org Foundation and contributors) is included as source in [`third_party/opus/`](third_party/opus/) and compiled into the game when it is built (the game's music is Opus), under its [BSD license](third_party/opus/COPYING). [`third_party/README.md`](third_party/README.md) says which files; none is modified.

## SDL3

Controller input can use [SDL3](https://www.libsdl.org/) (Simple DirectMedia Layer), copyright Sam Lantinga, under the [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt).

SDL3 is an external dependency: this repository contains no SDL source or binaries. The runtime is compiled against the headers of the official SDL3 Windows development package (`deps/SDL3`, which `Install WindowSmash.bat` downloads from SDL's GitHub releases, or `SSBU_SDL3_DIR`) and loads `SDL3.dll` from the executable's folder at run time. A package that includes `SDL3.dll` should include that package's `LICENSE.txt` too; the local bundler (`tools/bundle_ssbu.py`) adds it as `build/SDL3-LICENSE.txt`.

## Python

`Install WindowSmash.bat` may put a copy of [Python](https://www.python.org/) (the embeddable package from python.org, under the [PSF license](https://docs.python.org/3/license.html)) into `deps/python`, with [pip](https://pip.pypa.io/) and the packages below, when the PC has no Python it can use. Nothing from it is distributed with WindowSmash.

## The compiler: LLVM, and Microsoft's C runtime and Windows SDK

`tools/msvc_deps.py` (which `Install WindowSmash.bat` and `launchers\Build WindowSmash.bat` run when the PC has no compiler) downloads two external dependencies onto the PC that builds the game. Neither is part of this repository or distributed with WindowSmash.

- **LLVM** (`clang-cl`, `lld-link`, `llvm-lib` and clang's headers) into `deps/llvm`, from LLVM's official release on GitHub, under the [Apache License 2.0 with LLVM exceptions](https://llvm.org/LICENSE.txt).
- **Microsoft's C runtime and Windows SDK** (headers and 64-bit libraries) into `deps/msvc`, from Microsoft's servers, under Microsoft's license terms for the Visual Studio Build Tools, which the tool shows and asks you to accept first. They are Microsoft's software, used to build the game on that PC; they are not for passing on. The game built with them links Microsoft's static C runtime, as it does when built with Visual Studio.

## Other external dependencies

These are external dependencies, not bundled implementations. Their own licenses apply when you obtain, link or distribute them.

| Dependency | Used for | How it's obtained |
| :--- | :--- | :--- |
| SDL3 | Controllers other than Xbox pads | Optional, the official Windows development package. |
| PyCryptodome (`pycryptodomex`) | Reading game packages | `requirements.txt`. |
| zstandard | Reading `data.arc` on Pythons older than 3.14 | `requirements.txt` (Python 3.14 has `compression.zstd` built in). |
| Capstone, Unicorn | Disassembly and differential tests | `tests/requirements-dev.txt`, optional. |

A developer can link their own libopus or libvpx builds instead of the included sources (`SSBU_OPUS_LIBS`, `SSBU_VPX_DIR`: [`tools/cc.py`](tools/cc.py)). No binaries of any library are checked into this repository.

## Game content and names

The Git source tree contains no Nintendo game binaries, translated game functions, assets, fonts, keys, firmware or personal saves. Runtime package assets contain the precompiled executable and relocated module images; the original game-data archives, firmware fonts, keys and saves remain separate inputs.

Super Smash Bros. Ultimate and associated names are the property of their respective owners. WindowSmash is an independent project and is not affiliated with Nintendo.
