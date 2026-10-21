# How WindowSmash works

[← README](../README.md) · [Setup guide](BUILDING.md) · [Multiplayer](MULTIPLAYER.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md) · [13.0.5 update](UPDATES.md)

WindowSmash is a static recompiler plus a Windows compatibility runtime, specialized for this game. CPU code is translated before launch. GPU programs are translated when encountered. Selected operating-system and SDK entry points are implemented natively, while the remaining game and SDK code executes as compiled guest functions.

This document describes the checked-in implementation. Implemented paths and developer test tools should not be read as a compatibility guarantee for every mode, fighter, stage, or machine.

## Contents

| Section | Covers |
| :--- | :--- |
| [1. From extracted modules to a Windows executable](#1-from-extracted-modules-to-a-windows-executable) | Inputs, function discovery, translation, module placement, the game version, the imports check. |
| [2. Loading, symbols, and guest execution](#2-loading-symbols-and-guest-execution) | Mapping, binding, native replacements, exceptions. |
| [3. Native services](#3-native-services) | The runtime's service implementations, files and fonts. |
| [4. The graphics path](#4-the-graphics-path) | GPU commands, the D3D11 back end, shader compilation, audio and video. |
| [5. Multiplayer transport](#5-multiplayer-transport) | The relay, the lobby and direct connections. |
| [6. DLC and fighter unlocks](#6-dlc-and-fighter-unlocks) | Installed-content reporting and save editing. |
| [7. Tool reference](#7-tool-reference) | Every tool in `tools/`. |
| [Verification and debugging](#verification-and-debugging) | Tests, the overlay, reports and diagnostic switches. |
| [Build boundaries](#build-boundaries) | What is source, and what stays local. |

## 1. From extracted modules to a Windows executable

```mermaid
flowchart TD
    A[Your Program NCA / patched RomFS] --> B[nca.py: ExeFS and RomFS access]
    B --> C[nso.py: module loading and metadata]
    B --> D[arc.py: embedded fighter NRO extraction]
    C --> E[funcs.py: discover function boundaries]
    D --> E
    E --> F[lift.py + lift_simd.py: emit C]
    F --> G[cc.py: clang-cl compile and link]
    H[runtime/*.c: native host services] --> G
    G --> I[build/ssbu.exe]
    C --> J[dump/modules/*.reloc.bin]
    J --> I
```

### Inputs and discovery

[`nca.py`](../tools/nca.py) reads package headers, ExeFS containers, and RomFS tables using keys supplied locally. [`nso.py`](../tools/nso.py) handles module decompression and dynamic metadata, including imports, exports, relocations, and information useful for ELF conversion.

[`funcs.py`](../tools/funcs.py) finds function entries from exception-frame tables, exported function symbols, relocations into executable memory, initialization arrays, direct call targets, and recognizable import stubs. These sources provide more structure than a linear disassembly alone. Exception metadata also supplies extents used by the recompiler.

Fighter modules live inside `data.arc`. [`arc.py`](../tools/arc.py) resolves hashed archive paths and reads the compressed filesystem metadata. Its `nro` command extracts known modules from `prebuilt:/nro/release/`. The known-name list is 13.0.5's 98 modules.

### Translation

[`recomp.py`](../tools/recomp.py) orchestrates image preparation, C emission, incremental compilation, and linking. Each guest function becomes a C function accepting a `Cpu *` context. [`lift.py`](../tools/lift.py) emits integer/control-flow operations; [`lift_simd.py`](../tools/lift_simd.py) handles AdvSIMD/NEON operations with explicit vector lanes.

[`runtime/recomp.h`](../runtime/recomp.h) defines the shared contract:

- 31 general registers, the guest stack pointer, 32 vector registers, FP control/status, and thread-local register state.
- Guest memory operations expressed as host accesses to the mapped guest addresses.
- Function lookup for guest code pointers and cached indirect branch targets.
- Helpers for atomics, arithmetic, floating-point behavior, traps, and exception transitions.

Direct branches can target generated C labels/functions. Indirect calls resolve guest code addresses to native function pointers. Tail-call helpers keep supported guest tail transfers from accumulating unnecessary host stack frames. Unsupported instructions or unavailable targets can produce a trap; they do not establish complete instruction coverage.

The build disables floating-point contraction and fast-math-style assumptions to preserve guest arithmetic behavior. `clang-cl` produces x64 object files, then links them with the native runtime and Windows libraries.

### Stable module placement

The primary modules have explicit guest bases in `recomp.py`: `main`, `subsdk0`, and `sdk`. Extracted NRO modules receive aligned address ranges, recorded locally in `dump/nro/bases.txt`. The generated registry binds build IDs to the matching compiled module.

That address manifest is a build input. Preserve it when incrementally rebuilding an existing installation; generated code and relocation images must agree on placement.

Each module's build folder has a `module.txt` stamp: build ID, address, name, and a fingerprint of the tools that generated it (the recompiler's sources and the runtime's list of native functions). With the same stamp, [`modcache.py`](../tools/modcache.py) leaves the generated C as it is, and the build only checks it for objects to compile. After a change to the recompiler, the C is generated again, but only changed files are compiled.

### The game version

WindowSmash builds one version of the game: 13.0.5, its last update. [`game_version.py`](../tools/game_version.py) holds what identifies it: the main module's build ID (`21450C647B8C5940ED5B68218EA128FD3C5D597D`), the update's title version (`0x1F0000`, v31) and data.arc's version field (`0xD0005`). Each step checks what it can see:

| Where | What it checks |
| :--- | :--- |
| A game folder's scan ([`gamefiles.py`](../tools/gamefiles.py), shown in the installers' windows) | The update's title version, else data.arc's version, else the version in the game's control data. Files that name another version get a message saying what to add; files that name none are left to step 4. |
| [`build_game.py`](../tools/build_game.py) step 4 | The build ID of the `main` it copied into `dump/exefs`. Another version's is removed again, and the build stops. |
| `build_game.py` step 6 | data.arc's version in the connected game data ([`arc.py`](../tools/arc.py)), which also catches a `romfs.bin` made by hand from another version. |
| The game, as it starts ([`host.c`](../runtime/host.c)) | Its own `main`'s build ID. A game built from another version shows a plain message and exits with code 4: no crash report. |
| The online lobby ([`ssbu_relay.py`](../tools/ssbu_relay.py)) | The build ID in each join. Another is refused, with a reason the game shows in its overlay: `Netplay: WindowSmash needs game 13.0.5: run the installer again`. |

The runtime and the lobby keep their own copy of the build ID (the lobby is deployed on its own); `tests/test_game_version.py` checks that the copies match.

A folder whose `dump/exefs/main` is another version's, set up before WindowSmash required 13.0.5, is set up again by `build_game.py` once the game folder has 13.0.5: it removes what the game files made (the program, the game data's configuration and the update's files, the DLC list, the fighter modules, `ssbu.exe`) and runs those steps again. The save, the settings, the fonts, a decrypted base and the shader cache stay, and module build folders are generated again where their build ID changed ([the 13.0.5 update](UPDATES.md#a-folder-built-from-another-version)).

### Code tied to the game version

- **Hooks in the runtime** (`HLE_WRAP(<address>)`, in `runtime/hle_fiber.c`, `hle_movie.c`, `hle_net.c` and `hle_online.c`) wrap a guest function that isn't exported, at its 13.0.5 address. Each has a code signature on the line above it: its first instructions, with addresses masked out. The recompiler checks that a function starts at that address and begins as its signature says; if not, the build stops rather than leave the hook out. `tools/recomp.py wrap-sig <module> <address>` prints a new hook's signature.
- **Unlock All Fighters** writes at 13.0.5's save offsets. It reads main's build ID and changes nothing unless it is 13.0.5's (`LAYOUTS` in `tools/unlock_all.py`, `$knownLayouts` in `tools/Unlock-All-Fighters.ps1`).

### The imports check

Before compiling, `recomp.py all` lists each module's imported symbols that no module exports and the runtime doesn't implement, in `build/imports.unresolved.txt`. `recomp.py check <exefs>` runs that check on its own.

- A **strong** import on that list would trap if called: the runtime lacks a native function the game needs.
- **Weak** ones read as 0 and are normal. 13.0.5 has 0 strong and about 410 weak.

## 2. Loading, symbols, and guest execution

[`host.c`](../runtime/host.c) maps `*.reloc.bin` images at their expected guest addresses, registers module ranges, publishes exports, resolves imports, and applies symbol relocations. Function metadata associates guest addresses with compiled host functions.

Native replacements use `HLE(symbol)` declarations. The build detects those names and redirects applicable guest entry points to runtime implementations. Unresolved strong imports receive diagnostic handlers rather than silently acquiring arbitrary implementations.

Startup enters the guest SDK initialization path and eventually `nnMain`. Guest execution runs on a Windows thread with a large reserved stack. Modules loaded later through `nn::ro` are matched by build ID, mapped, bound, and initialized when requested.

[`hle_ro.c`](../runtime/hle_ro.c) can save an unrecognized module image into the local NRO dump directory. Harvesting a module does not compile it: a later rebuild must translate and link it. The runtime's missing-module behavior is a development aid, not a substitute for preparing the full module set.

### Exceptions and non-local control flow

The guest's own unwind implementation interprets guest exception metadata. [`hle_unwind.c`](../runtime/hle_unwind.c) bridges its final register/context transfer to the corresponding recompiled host frame using a structured host exception and landing-pad filters. [`hle_setjmp.c`](../runtime/hle_setjmp.c) and [`hle_fiber.c`](../runtime/hle_fiber.c) implement related control-flow bridges.

## 3. Native services

| Source | Responsibility |
| :--- | :--- |
| [`kernel.c`](../runtime/kernel.c) | Guest thread contexts, synchronization, handle/state tracking, memory operations, and supported supervisor calls. |
| [`clock.h`](../runtime/clock.h) | The one clock behind the guest's system tick, the kernel's timeouts, and the renderer's timing: Windows' `QueryPerformanceCounter`, or, where a read of it is slow (a PC timed by the HPET), the CPU's invariant time stamp counter scaled to the same nanoseconds. `SSBU_CLOCK=qpc` or `tsc` picks one; `build\play.log`'s `[clock]` line says which. |
| [`ipc.c`](../runtime/ipc.c), `ipc_*.c` | Service request handling and selected audio, display, Mii, and related interfaces. |
| [`hle_fs.c`](../runtime/hle_fs.c) | RomFS reads, save/cache mounts, and host file operations. |
| [`hle_env.c`](../runtime/hle_env.c), [`hle_applet.c`](../runtime/hle_applet.c) | Account/environment behavior, applet-facing functions, and installed add-on reporting. |
| [`hle_hid.c`](../runtime/hle_hid.c), [`hle_pad.c`](../runtime/hle_pad.c) | Up to eight players' pads, keyboard input, rumble, and optional scripted input. An input thread owns SDL3 (loaded from `SDL3.dll` at run time; XInput pads 1-4 without it) and copies each pad's state for the game's threads; `controllers.ini` maps pads to players and buttons. GameCube controllers (Nintendo's adapter, through SDL3's own driver) report the console's GameCube style, with analog triggers and an on/off motor, so the game uses its GameCube controls. On Windows, [`hle_padusb.c`](../runtime/hle_padusb.c) reads Nintendo's GameCube adapter (WinUSB or Windows' HID driver) and the Switch 2's GameCube controller (started through its bulk interface) itself, through Windows' own libraries, because SDL3.dll has no libusb; SDL is told to ignore both (`SSBU_GC_USB=0` leaves them to SDL). |
| [`hle_pl.c`](../runtime/hle_pl.c) | Shared-font loading from locally supplied `.bfttf` files. |
| [`hle_nv.c`](../runtime/hle_nv.c) | Supported GPU-driver interfaces, address mapping, and channel operations. |
| [`hle_net.c`](../runtime/hle_net.c), [`net_relay.c`](../runtime/net_relay.c) | Virtual networking, Local Wireless, socket behavior, relay transport, and optional UPnP. |
| [`hle_online.c`](../runtime/hle_online.c) | The game's Online mode: NEX and Pia sent to the online lobby's game server, NEX's TCP streams carried to the lobby, and the SHA-1 its WebSocket needs. |
| [`hle_offline.c`](../runtime/hle_offline.c) | Selected offline service behavior needed by the target game. |

These are targeted service implementations, not a complete implementation of the console OS. Some functions return fixed or limited behavior appropriate to the supported paths.

### Files and fonts

`dump/modules/romfs.cfg` contains two lines: a host file path, then the byte offset at which an unencrypted RomFS begins. Relative paths resolve against the game's working directory. The runtime reads RomFS metadata and game files in place rather than extracting all assets.

Save data and cache files are host directories. `SSBU_DATA_ROOT` can redirect those per-run files for testing. A network identity is separate from save contents: copying `netplay_id.txt` to another PC can make two installs appear as the same player.

The font service loads six firmware font files, decodes their container format, and maps shared font memory. Fonts are user-supplied runtime inputs, not repository assets.

## 4. The graphics path

```mermaid
flowchart LR
    A[Guest GPU submission] --> B[GPU virtual memory / channels]
    B --> C[GPFIFO method streams and MME macros]
    C --> D[Graphics / compute state]
    D --> E[D3D11 backend]
    F[Maxwell shader] --> G[Shader translator: HLSL]
    G --> H[Compiler workers / DXBC cache]
    H --> E
    E --> I[Swapchain / overlay / captures]
```

[`gpu.c`](../runtime/gpu.c) parses Tegra/Maxwell command streams, engine bindings, method writes, macros, transfers, synchronization, and dispatch/draw events. [`gpu_host1x.c`](../runtime/gpu_host1x.c) covers the Host1x/video-related job path. The backend interface separates command/state handling from rendering.

As on the console, the GPU runs alongside the CPU. A submission is copied into a queue and the driver gets its fence back at once; a GPU thread of its own ([`gpu_queue.c`](../runtime/gpu_queue.c)) runs the queue in order, and a submission's sync point reaches its fence when it has run, which is what the driver's fence waits wait for. A buffer queued for display is shown once everything submitted before it has run, and unmapping GPU addresses or decommitting memory that holds GPU allocations waits for the queue first. Rendering inside the submitting call instead put all of it on the game's main thread (in an 8-player match about 60% of that thread); `SSBU_GPU_THREAD=0` still does that, for comparison.

[`gpu_d3d11.c`](../runtime/gpu_d3d11.c) creates D3D resources, tracks render targets and pipeline state, uploads or retains guest buffers, handles graphics/compute work, and presents the result. [`gpu_display.c`](../runtime/gpu_display.c) owns the window (windowed, borderless, or full screen, from `display.ini`) and the pacing of presents: a 60 Hz high-resolution clock, the same clock presenting with tearing allowed for variable-refresh monitors, or the monitor's own refreshes. The game's 1920 x 1080 picture is drawn first, then scaled to the window at 16:9. With `display.ini`'s `render_scale`, every render target is created that many times the game's size: the game still works in its own pixels, and the renderer scales what is given in pixels (viewports, scissors, 2D-engine blits) to each target's size. Shaders sample with normalized coordinates, so almost none need to change; the few that read their pixel's position or step in texels (texel offsets, texel fetches) get the scale through spare words of the per-draw constants, and those few cache their compiled code under a salted hash so files from before scaling are not used for them. A picture bigger than the window is averaged down to it in linear light. [`gpu_texture.c`](../runtime/gpu_texture.c) contains texture-format/layout helpers. [`gpu_shader.c`](../runtime/gpu_shader.c) decodes guest shader instructions and emits HLSL.

### Shader compilation and the driver fix

CPU code is compiled ahead of time; GPU shaders remain demand-driven. A new shader is translated to HLSL, then queued for compilation. Persisted DXBC cache loads have priority over new HLSL compiles.

The current implementation uses up to 16 dedicated compiler workers with 64 MiB reserved stacks. The `SSBU_COMPILE_THREADS` environment variable controls the worker count within the implementation's limits. Shader cache entries include a translation-version marker and relevant shader/state hashes.

The shader fix addresses driver compilation on small internal worker stacks. A second D3D11 device, created with internal threading optimizations disabled, warms driver compilation on WindowSmash's own compiler threads. The rendering device retains its normal threading behavior. Compilation stages and current jobs are retained for crash reporting.

At start, the compiler threads create every cached DXBC program of the current cache version that the game has used, on both devices, on a queue below the game's own loads and compiles (`SSBU_SHADER_PREWARM=0` turns this off). When the game first uses such a program, it is still translated for its interface, but the prepared object is taken instead of a new compile, so its draws are not skipped. [`gpu_shader_cache.h`](../runtime/gpu_shader_cache.h) holds what the renderer and the ahead-of-time compiler must agree on: the cache version, the program hash, the file name, and the compiler flags. [`shader_aot.py`](../tools/shader_aot.py) finds the game's programs in `data.arc` and compiles them into the cache in advance; its files stay dated 2001 until the game first uses them, and the start-up preparation leaves them out until then.

AMD's Direct3D 11 driver makes a program's GPU code on threads of its own, not inside the call that creates the program or the draw that first uses it, and the GPU waits for that code. The driver keeps that code in a disk cache under the executable's name, so a program it has made before costs about a millisecond; one it has not made is a full compile, tens of milliseconds or more, the first time a move or its effect needs it. Programs the game has played are in that cache already. Programs that Prepare Shaders compiled but the game has not used yet are not. With AMD's driver, when the game closes it starts `ssbu --warm-shaders` in the background. That process draws every program in the shader cache once on Direct3D devices of its own, played programs first and then Prepare Shaders' programs, so the driver's cache has them before the game needs them. It runs in Windows' background mode, stops as soon as a game starts, and continues after the next one. It records what it has drawn in `warm.done` in the shader cache and draws everything again after a driver or executable change. Its result goes to `warm.log` in the shader cache, and the next start shows that result in the log. `SSBU_SHADER_WARM=0` turns this off, and `=1` turns it on with any driver. `=2` also draws each program that the start-up preparation makes once, into a 1x1 target with a partner shader matching its interface. That was the default before. On AMD, though, the driver already had nearly all of those programs cached, and its threads were still busy well into the first match. Every session's log also has two related `[status]` lines. `first draws with a shader` gives how many draws first used a shader and how many of those took the renderer over 4 ms. `graphics driver's own threads` gives how much processor time the driver's threads took since the previous status line.

This is a targeted mitigation, not a claim of driver-independent behavior. Rendering work with unavailable shaders can be skipped while compilation is pending. First-use hitches, incomplete visuals, or translation failures still need investigation on each supported path.

### Audio and video

`ipc_audio.c` supplies the audio-service path. The game appends short buffers (10.67 ms each, about five at a time) and mixes the next one when one is reported played; the runtime copies them into its own queue, the output device (WASAPI on Windows, waveOut if WASAPI cannot start; SDL on Linux and macOS) takes from that queue at its own pace, and a buffer is reported played once the device is within 20 ms of its end, so a late report never leaves the device without sound. `SSBU_AUDIO_LATENCY_MS` changes those 20 ms, `SSBU_AUDIO=waveout` picks waveOut, `SSBU_AUDIO_LOG=1` logs every buffer, and the log says when the game's sound ran out. `ipc_opus.c` uses a statically linked libopus. The video path reconstructs VP9 frame information and decodes with libvpx. [`gpu_vp9.c`](../runtime/gpu_vp9.c) contains frame/header logic; its default probability tables carry [libvpx attribution](../NOTICE.md).

Both codecs are built from the sources in [`third_party/`](../third_party/README.md) by [`codec_libs.py`](../tools/codec_libs.py) when the program is linked (`cc.py` adds `SSBU_HAVE_OPUS` and `SSBU_HAVE_VPX`); a developer's own builds take their place through `SSBU_OPUS_LIBS` and `SSBU_VPX_DIR`. Without either, the game builds and runs with that codec silent.

## 5. Multiplayer transport

The game continues to run its own Local Wireless session logic. WindowSmash presents a virtual LAN and carries UDP payloads through a relay. This does not replace the game's netcode with rollback networking.

[`net_relay.c`](../runtime/net_relay.c) implements both the game-side client and the integrated host relay of a private room. [`ssbu_relay.py`](../tools/ssbu_relay.py) is the online lobby: the same transport, plus room lists. The default UDP port is `39420`, and virtual addresses are allocated within `10.13.0.0/16`.

The protocol (version 2) uses a short magic/version/type header, big-endian fields, join/welcome/member messages, keepalive ping/pong, forwarded data, leave messages, and the lobby's list/listing/list-end messages. A join carries the main module's build id; the lobby lets in 13.0.5's only ([the game version](#the-game-version)). A private relay's rooms hold at most eight members; the game's selected mode can impose a smaller player limit. Peers silent for ten seconds are removed.

The lobby gives every player an address of its own and partitions players by build id and lobby code (`SSBU_NET_ROOM`; `smash`, the default, is the public lobby). `nn::ldn`'s scan asks the lobby for its room list and waits up to 500 ms for it. Room membership is derived from the Local Wireless messages the lobby relays, and a room's traffic passes only between that room's players. The client accepts join answers only from the host it asked, and adverts, departures and kicks only from the network's host. A lobby that has not answered for five seconds counts as offline: Local Wireless then reports itself unavailable, as without netplay.

Direct connections keep protocol version 2 and are negotiated: a lobby that introduces players sets a flag in its welcome, and a game that can connect directly sends a longer ping; older games and lobbies ignore both. The lobby sends each player a peer list of its room (type 12). A pair is introduced only when both allow it and their games have already exchanged game packets through the lobby; it then gets each other's public address as the lobby sees it (plus the LAN address when both share a public IP) and a 64-bit secret. Punch hellos and acks (type 14) carry nonces, and a path counts only once an ack echoes a recent hello from the address it went to. Game packets then travel as type 15 with the pair's secret, and the receiving game delivers one only from a lobby-given address, with the sender's own virtual address, on a game port, from a node of its Local Wireless network. Broadcasts go to the lobby as type 13, which forwards them to the room's players the sender does not reach directly. Local Wireless's own control messages always go through the lobby. A pair falls back to the lobby when punching gets no answer within 5 s, or when a direct path has had no ack for 1.2 s; hellos continue so a path can recover. `SSBU_NET_DIRECT=0` (or `off` in `online_direct.txt`) opts out, and the lobby then shares that player's address with nobody.

The game's Online mode logs in to a NEX game server of WindowSmash's own, [`tools/nex`](../tools/nex/) (standard-library Python), which [`ssbu_online_lobby.py`](../tools/ssbu_online_lobby.py) runs inside the lobby at the lobby's virtual address, `10.13.0.1`. With netplay configured, [`hle_env.c`](../runtime/hle_env.c) gives the user a network service account: an id derived from `netplay_id.txt`, and an id token with that id and the player name, which only this server reads. Hooks in [`hle_online.c`](../runtime/hle_online.c) give NEX a direct server address, with which it opens its WebSocket without TLS, and resolve Nintendo's server names (the game server, Pia's NAT check servers) to the lobby; nothing goes to Nintendo. NEX's stream socket travels over a TCP connection to the lobby's port, and the game server answers login, matchmaking (MatchmakeExtension, MatchMaking, NAT traversal), ranked ratings and the Online menu's other calls. Matched players' Pia traffic then crosses the virtual LAN as Local Wireless's does, relayed by the lobby only between players the game server put in one match. The server lets a connection act only for the player it logged in as and on gatherings that player belongs to, and bounds connections, buffers and stored state. `python -m nex.selftest`, run in `tools`, tests it without a game.

Lobby codes and room names are routing identifiers, not passwords. The protocol is not encrypted or an authenticated public service. Use it with trusted players and a reachable relay. A virtual LAN can simplify connectivity when the host cannot accept inbound UDP directly. Follow the [multiplayer setup guide](MULTIPLAYER.md).

## 6. DLC and fighter unlocks

13.0.5's content archive already contains the assets and code of every DLC fighter. `aoc.cfg` reports locally installed add-on indices to `nn::aoc`; it does not add game data. DLC fighter NROs must also be extracted and compiled.

The unlock tools edit 13.0.5's save layout; they check that the folder's program is 13.0.5's (its build ID), validate the fighter tables, and create a backup. Base-game selection flags and associated milestones are separate from installed DLC reporting. Mii Fighters may still need to be created in-game. See the [DLC and unlocks guide](DLC-AND-UNLOCKS.md) for usage and limitations.

## 7. Tool reference

Paths below are relative to the repository root. `tools/` holds what the installer, the build and the launchers run; `tools/dev/` holds the developers' analysis, verification and automation tools, which may require `tests/requirements-dev.txt` and locally prepared game inputs; `tools/linux/` is the Linux build ([Linux](LINUX.md)). Several modules are libraries imported by the driver rather than general user commands.

### Build and content preparation (`tools/`)

| Tool | Role |
| :--- | :--- |
| [`nca.py`](../tools/nca.py) | Inspect packages; extract ExeFS/RomFS files; generate `romfs.cfg` and installed-content `aoc.cfg`. |
| [`arc.py`](../tools/arc.py) | Inspect `data.arc`, extract named paths, and collect known embedded NRO modules. Uses Python 3.14's `compression.zstd`, or the `zstandard` package on older Pythons. |
| [`gamefiles.py`](../tools/gamefiles.py) | Say what a folder of game files holds (the guide's folder or an emulator's installed-content folder): the program to build from, base or update, DLC, versions, and whether it is 13.0.5. |
| [`game_version.py`](../tools/game_version.py) | The one game version, 13.0.5: its main build ID, update title version and data.arc version, and the messages for a game that isn't 13.0.5 ([the game version](#the-game-version)). |
| [`build_game.py`](../tools/build_game.py) | The one-command build behind `launchers\Build WindowSmash.bat`: tools check, packages, program (checked to be 13.0.5's), fonts, data (decrypting or applying the 13.0.5 update when needed; data.arc's version checked), DLC, fighters, build. A folder set up from another version is set up again. |
| [`codec_libs.py`](../tools/codec_libs.py) | Build libopus and libvpx's VP9 decoder from `third_party/` into static libraries, when they are missing or stale ([third-party sources](../third_party/README.md)). |
| [`install_windowsmash.ps1`](../tools/install_windowsmash.ps1) | What `Install WindowSmash.bat` runs: PC checks, Python, the compiler, SDL3, the folder wizard, then `build_game.py`. |
| [`update.py`](../tools/update.py) | Apply the 13.0.5 update over the game's data as a layered RomFS in `dump/`, checking every block against the update's hashes ([the 13.0.5 update](UPDATES.md)). |
| [`modcache.py`](../tools/modcache.py) | Keep fighter module addresses (`dump/nro/bases.txt`), the stamp that says a module's build folder is current, and the imports check. |
| [`nso.py`](../tools/nso.py) | Read NSO/NRO images and dynamic metadata; inspect modules and export ELF representations. |
| [`funcs.py`](../tools/funcs.py) | Discover functions and optionally write a function inventory. |
| [`recomp.py`](../tools/recomp.py) | Run `coverage`, `emit`, `image`, `build`, `compile`, `link`, the end-to-end `all` driver, the `check` of unresolved imports, or `wrap-sig` for a hook's code signature. |
| [`lift.py`](../tools/lift.py) | AArch64 integer/control-flow lifting implementation. |
| [`lift_simd.py`](../tools/lift_simd.py) | NEON/AdvSIMD lifting implementation. |
| [`a64.py`](../tools/a64.py) | Shared instruction-emission helpers, register expressions, types, and unsupported-operation handling. |
| [`cc.py`](../tools/cc.py) | Find `clang-cl`, construct compile options, compile incrementally, and link Windows executables. |

### Analysis and diagnostics (`tools/dev/`)

| Tool | Role |
| :--- | :--- |
| [`disasm.py`](../tools/dev/disasm.py) | Disassemble an address range, optionally beside lifted C output. |
| [`calls.py`](../tools/dev/calls.py) | List a function's direct calls and name imported callees. |
| [`census.py`](../tools/dev/census.py) | Summarize imported/exported SDK boundaries and dynamically requested NVN entry points. |
| [`insn_census.py`](../tools/dev/insn_census.py) | Count decoded instruction forms in a module's text. |
| [`ipc_reach.py`](../tools/dev/ipc_reach.py) | Trace direct-call reachability to service IPC boundaries; indirect calls make its result a lower bound. |
| [`eh.py`](../tools/eh.py) | Inspect exception-frame information and C++ landing-pad tables. |
| [`find_crc.py`](../tools/dev/find_crc.py) | Find the CRC-32 table and code references to it. |
| [`strrefs.py`](../tools/dev/strrefs.py) | Report strings referenced by instructions in a range. |
| [`sym.py`](../tools/dev/sym.py) | Resolve module-relative offsets to nearby exported function symbols. |
| [`hostsym.py`](../tools/dev/hostsym.py) | Resolve host executable offsets using a locally generated linker map. |
| [`sass.py`](../tools/dev/sass.py) | List or count Maxwell shader instructions in dumped shader binaries. |
| [`hlslc.py`](../tools/dev/hlslc.py) | Compile an HLSL file through Windows D3DCompiler and report diagnostics. |
| [`shot.py`](../tools/dev/shot.py) | Convert captured BMP frames to PNG and summarize colors. |

### Verification and automation (`tools/dev/`)

| Tool | Role |
| :--- | :--- |
| [`difftest.py`](../tools/dev/difftest.py) | Compare lifted single-instruction behavior against Unicorn across sampled CPU states. |
| [`functest.py`](../tools/dev/functest.py) | Compare selected whole leaf functions against Unicorn, including registers and memory writes. |
| [`shadertest.c`](../tools/dev/shadertest.c) | Translate dumped guest shaders and compile their HLSL with D3DCompiler. |
| [`padtest.c`](../tools/dev/padtest.c) | Check the controller layer offline: configuration, button mapping, GameCube controllers, sticks, player slots, and rumble pacing. |
| [`clocktest.c`](../tools/dev/clocktest.c) | Check the clock offline: the timer it picks (also with a system timer slowed to a microsecond a read), that it never goes back across threads, and its rate and cost (`tests/test_clock.py` builds and runs it). |
| [`vp9test.c`](../tools/dev/vp9test.c) | Replay recorded NVDEC jobs through the VP9 path and libvpx. |
| [`vp9rt.c`](../tools/dev/vp9rt.c) | Exercise VP9 header reconstruction with an independently parsed synthetic clip. |
| [`menurun.sh`](../tools/dev/menurun.sh) | Script menu navigation, captures, and logging using a copied test save. |
| [`autobattle.sh`](../tools/dev/autobattle.sh) | Drive a timed menu-to-battle input sequence for local diagnostics. |
| [`netpair.sh`](../tools/dev/netpair.sh) | Run two local instances through loopback relay transport with separate copied test data. |
| [`smoke.ps1`](../tools/dev/smoke.ps1) | Compile and execute a small known game-function smoke harness against prepared local inputs. |

### Play, package and serve (`tools/`)

| Tool | Role |
| :--- | :--- |
| [`shader_aot.py`](../tools/shader_aot.py), [`shaderaot.c`](../tools/shaderaot.c) | Find the game's shader programs in `data.arc` and compile them ahead of time into the disk cache, as the renderer would (`launchers\Prepare Shaders.bat`). |
| [`ssbu_relay.py`](../tools/ssbu_relay.py) | Run a standalone UDP relay without the game, native runtime, or game files. |
| [`ssbu_online_lobby.py`](../tools/ssbu_online_lobby.py), [`nex/`](../tools/nex/) | Run the online lobby with the game server for Online mode (UDP and TCP on one port); `python -m nex.selftest` tests the server. |
| [`unlock_all.py`](../tools/unlock_all.py) | Validate, back up, and unlock the compatible local save using Python. |
| [`Unlock-All-Fighters.ps1`](../tools/Unlock-All-Fighters.ps1) | Equivalent Windows save-unlock helper, with a running-game guard and atomic backup/replacement. |
| [`Configure-Game-Data.ps1`](../tools/Configure-Game-Data.ps1) | Configure a runtime release using a separately supplied RomFS, fonts, and optional installed-content config. |

The `.sh` helpers expect a Unix-like shell/tool environment that can run the Windows executable. They recreate their named output/test directories; use disposable capture paths. They are developer scripts, not required player launchers.

## Verification and debugging

Source-only checks:

```powershell
python -m unittest discover -s tests -v
```

Game-backed checks after preparing local modules:

```powershell
python tools/recomp.py coverage dump/exefs/main --jobs 8
python tools/dev/difftest.py dump/exefs/main --jobs 8
python tools/dev/functest.py dump/exefs/main --jobs 8
```

Differential results apply to the sampled encodings, states, and functions. Whole-function tests deliberately skip reference runs that cannot complete in isolation; neither test is proof that every game path works.

The overlay exposes frame/rendering counters. F1 toggles the overlay, F2 shows details (with the connected pads), F3 captures a frame, F4 requests draw diagnostics, F6 triggers a sampling profile, and F11 switches to full screen.

A host exception writes `reports/crash_<time>.txt` with a minidump, and a runtime stop writes `reports/stopped_<time>.txt`. The minidump is written by a second process (the same executable started with `--write-dump`), because a process that dumps itself can deadlock on a lock held by one of its suspended threads. A watchdog ([`debug_watchdog.c`](../runtime/debug_watchdog.c)) writes `reports/freeze_<time>.txt` and a minidump when no frame has been presented for 5 s (again at 30 s and 120 s): the owners of the renderer's, present queue's, kernel's, and GPU channel's locks, the shader compiles in flight, the guest threads' states, and every thread's stack, with guest functions named by module offset. `SSBU_WATCHDOG=0` turns the watchdog off; `SSBU_MINIDUMP=0` writes no minidumps and `SSBU_MINIDUMP=full` writes all memory. `SSBU_HITCH=<ms>` logs every frame longer than that with what the main, vsync, and renderer threads spent it on: the program's functions by name once its map has been read (a few seconds after start), and for a wait inside a driver or library, the program's function that called into it (`... via <function>`). Locally generated linker maps are useful to `hostsym.py` and profiling tools, but are not required to run a release build.

Useful environment variables include `SSBU_DATA_ROOT`, `SSBU_SHADER_CACHE`, `SSBU_SHADER_PREWARM`, `SSBU_SHADER_WARM`, `SSBU_COMPILE_THREADS`, `SSBU_DUMP_SHADERS`, `SSBU_DUMP_HLSL`, `SSBU_INPUT`, `SSBU_CAPTURE`, `SSBU_CONTROLLERS` (another `controllers.ini`), `SSBU_PAD_BACKEND=xinput`, `SSBU_DISPLAY` (another `display.ini`), `SSBU_WINDOW`, `SSBU_WINDOW_SIZE`, `SSBU_PACING`, `SSBU_RENDER_SCALE` (`display.ini`'s `render_scale`), `SSBU_SCALE_SKIP` (render target sizes kept at 1x, e.g. `square,960x540`, to find which one a scaling artifact comes from), `SSBU_GPU` (`display.ini`'s `gpu`; `warp` for Windows' software renderer), `SSBU_D3D_DEBUG` (Direct3D's debug layer, its messages in the log; needs Windows' optional *Graphics Tools*), `SSBU_WATCHDOG`, `SSBU_MINIDUMP`, `SSBU_HITCH`, `SSBU_CLOCK`, and the [network variables](MULTIPLAYER.md#reference-environment-variables). Several other `SSBU_*` switches are implementation-level debugging controls; consult their use sites before enabling them.

## Build boundaries

Checked-in code defines the recompiler and native runtime. Local game inputs supply the guest program and assets. A runtime release contains the compiled executable/module images but still requires separately supplied game data and fonts. Generated C, binaries, caches, captures, and personal data stay out of the Git source tree.
