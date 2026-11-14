# Targets — Windows first, Android as the destination

The port exists to run **on a phone**, GDroid-style: a distributable engine
the user installs, points at their own copy of the game, and plays. Windows is
the bring-up platform because it has the complete rendering path; the game's
own code is identical on every target.

## Why Windows first

The toolkit's rendering layer is two backends over the same API:

| Target | Backend | Status of the layer |
|---|---|---|
| Windows 10/11 x64 | D3D11 (`d3d8_*.c`) | the complete, reference path |
| Linux / macOS | OpenGL 3.3 + SDL2 + libepoxy (`d3d8_gl.c`) | works, less exercised |
| **Android arm64** | OpenGL ES via SDL2 + libepoxy (same `d3d8_gl.c`) | **the destination** |

Bring-up order matters: a bug found on Windows is a bug in the *port*; the
same bug found on Android could be the port *or* the GL backend. Prove the
game on Windows (menus, gameplay, the whole boot chain), then bring the GL
path up for Android with a known-good reference on the other side of it.

## What is target-independent

Everything that matters:

* The **generated C code** (the game lifted to C) — Python output, no platform
  in it.
* The **kernel layer** (thunk bridge, memory layout, file path redirection,
  thread and heap models) — C11, builds with Clang anywhere.
* The **game's own logic** — CRT startup, XAPI, the save subsystem, the
  D3D8 lib inside the game binary.

## What a target adds

| Piece | Windows | Android |
|---|---|---|
| Window + context | Win32 + D3D11 swapchain | SDL2 activity + GLES surface |
| Rendering backend | `xbox_d3d8` (D3D11) | `xbox_d3d8` (GL/GLES path) |
| Input | XInput | SDL2 gamepad + touch overlay |
| Audio | DirectSound shim → XAudio2 | OpenSL/AAudio shim |
| Packaging | `.exe` + `game/` folder | APK (engine) + user-supplied `game/` on storage |
| Build | MSVC | Android NDK (Clang, arm64-v8a) via Gradle |

## Android roadmap (after the Windows boot chain is proven)

1. **NDK build of the runtime** — a CMake toolchain file for
   `arm64-v8a` (min SDK 26), SDL2 static for Android, libepoxy. The toolkit
   builds its Linux targets today; Android is Linux with an EGL surface and a
   different SDL2 package.
2. **Gradle module in this repo** (`android/`) — an SDL2 `SDLActivity`
   subclass, `jni/` CMake wiring, the native main entry (`main.c`'s host side
   already separates WinMain from `main`), storage-permission handling.
3. **Input layer** — the game reads the pad through the kernel's XAPI layer;
   map an SDL2 gamepad into the Xbox pad state, plus an on-screen overlay
   (stick + face buttons + triggers) since phones have no sticks.
4. **Game supply flow** — on first launch the app lets the user pick the
   extracted disc folder (`default.xbe` + `Media/`); `xbox_path_init`
   redirects the game's `D:\Media\...` opens there. Nothing copyrighted ships
   in the APK.
5. **CI** — an `android` job in the build repository: NDK + Gradle
   `assembleRelease`, signed with a keystore from secrets, APK published to
   the private artifact store (and, when legally structured for release, to
   the engine project's releases — engine only, never game code).

The Windows bring-up running in CI today is the reference that keeps step 1
honest: the same game booting on both targets is the proof.