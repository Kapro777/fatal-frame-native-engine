# Fatal Frame Native Engine

Static recompilation of **Fatal Frame (Xbox, 2002, Tecmo)** into a native
Windows executable, built on the
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

This repository holds the **engine that runs the game natively**: the host
entry point, the Xbox kernel/runtime bridge, the D3D8 abstraction, the memory
layout, the crash diagnostics, and the build glue. The game's own x86 code is
lifted to C on *your* machine from *your* copy of the game.

## Targets

**Windows** is the bring-up platform (complete D3D11 rendering path);
**Android (arm64)** is the destination — a distributable engine the user points
at their own copy of the game, like a native-port wrapper. The generated game
code is identical on both; only the rendering/input/packaging backend changes.
Full plan: [`docs/targets.md`](docs/targets.md).

## ⚖️ LEGAL — READ THIS FIRST

* **This repository contains no game code, no game assets, and no copyrighted
  material of any kind.** It is pure host-side engine code plus a build system.
* You must **own a legitimate copy of Fatal Frame for the original Xbox** to
  use this project. The toolkit contract and Xbox static recompilation in
  general require a legally acquired disc image.
* Nothing here grants you a license to distribute Fatal Frame, its code, its
  assets, or any executable derived from them. Built `.exe` files contain the
  game's code and are for personal use on the machine that built them.
* Fatal Frame is a trademark of its respective owners. This project is an
  independent, non-commercial preservation effort, **not affiliated with or
  endorsed by Tecmo, Microsoft, or any other rights holder**.
* By using this project you agree to comply with all applicable laws in your
  jurisdiction regarding reverse engineering and interoperability.

## What is static recompilation?

The original Xbox is an x86 machine. The game executable (`default.xbe`) is
x86 machine code. This project disassembles that binary, translates **every
function** into equivalent C source code, and compiles that C with MSVC into a
regular x86-64 Windows `.exe`. There is no emulator and no interpreter: the
game's own logic executes directly on your CPU, against the runtime layer in
this repository.

```
default.xbe ──parse──▶ disasm ──func_id──▶ abi_analysis ──recomp──▶ C code
                                                                      │
                                          this repo (engine + runtime) ◀┘
                                                   cmake + MSVC
                                                         │
                                                    fatalframe_native.exe
```

The heavy steps (pipeline + compilation) run in GitHub Actions in the
companion repository
[Kapro777/fatal-frame-xbox-recomp](https://github.com/Kapro777/fatal-frame-xbox-recomp),
not on your machine. See `docs/` there for the pipeline notes.

## Layout

```
CMakeLists.txt        Build glue: links the xboxrecomp toolkit + runtime libs
src/main.c            Host entry point: loads the XBE, maps Xbox memory,
                      installs the kernel bridge, calls xbe_entry_point()
src/recomp_manual.c   Hand-written function overrides + ICALL diagnostics
src/recomp/gen/       GENERATED CODE (gitignored) - produced from your XBE
docs/                 Per-title notes: entry point, memory layout, bring-up log
```

`src/recomp/gen/` never exists in git. The recompiler writes it, CMake globs it
with `CONFIGURE_DEPENDS`, and `.gitignore` keeps it out of the repository.

## Building it yourself

You need three things: this repo, the toolkit clone, and your own XBE.

```bash
git clone https://github.com/sp00nznet/xboxrecomp.git      # toolkit
git clone https://github.com/Kapro777/fatal-frame-native-engine.git
cd fatal-frame-native-engine

# 1. Extract default.xbe from your disc image into game/
mkdir game
#    (extract-xiso -x "Fatal Frame.iso" -d game/ , or xdvdfs, or the toolkit's
#     own extractor: py -3 -m tools.xiso unpack "Fatal Frame.iso" -o game/)
#    Also copy the full Media/ tree there: the game reads its assets at runtime.

# 2. Generate the C code from XBE (run inside the toolkit clone)
py -3 -m tools.xbe_parser    game/default.xbe --json game/fatalframe_analysis.json
py -3 -m tools.disasm        game/default.xbe --text-only -v
py -3 -m tools.func_id       game/default.xbe -v
py -3 -m tools.abi_analysis  game/default.xbe -v
py -3 -m tools.recomp        game/default.xbe --all --split 250 \
        --game-name "Fatal Frame" --gen-dir ../fatal-frame-native-engine/src/recomp/gen

# 3. Build the native executable (needs CMake 3.20+ and MSVC / VS 2022)
cmake -S . -B build -DXBOXRECOMP_DIR=../xboxrecomp -A x64
cmake --build build --config Release
build\Release\fatalframe_native.exe
```

Requirements: Python 3.10+ with `capstone`, CMake 3.20+, MSVC (Visual Studio
2022). Later XDK runtime notes are in `docs/title-profile.md`.

## Status

Early bring-up. The recompiled code boots through the game's real entry point
(`0x000DAB5D`, labeled `xbe_entry_point` by the pipeline) and the game runs its
own CRT startup, hardware init and menu code against the toolkit runtime.
Expect a crash-first, fix-loop bring-up — see the companion build repository's
CI logs for the current state.

## License

The engine code in this repository is MIT-licensed, derived from the
sp00nznet/xboxrecomp `templates/new-game/` (MIT). See `LICENSE`.