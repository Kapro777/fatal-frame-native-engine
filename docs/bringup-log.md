# Bring-up log

Running record of the Fatal Frame (Xbox) static recompilation bring-up.
Newest entries at the bottom.

## 2026-11-14 — Project scaffolded, first cloud build green

**Setup**
- Confirmed disc: Xbox XDVDFS, `default.xbe` 1,773,568 bytes, title
  `0x54430004`, English retail build (no Shift-JIS menu strings in the XBE).
- Three repositories created on the owner's account:
  - `fatal-frame-native-engine` (public) — engine + runtime + docs, ships no
    game code
  - `fatal-frame-xbox-recomp` (public) — CI build farm, no game code
  - `fatal-frame-xbe` (private) — the owner's `default.xbe`, input only
  - `fatal-frame-builds` (private) — derived `.exe` outputs, output only
- Engine project cloned from `templates/new-game/`, entry point
  `0x000DAB5D`, game dir `game/`.

**Pipeline, first cloud run (GitHub Actions, windows-latest, 2m49s)**
- `tools.xbe_parser` — OK (entry `0x000DAB5D`, 11 sections, XDK 4721,
  103 kernel imports)
- `tools.disasm` — **3,599 functions** detected
- `tools.func_id` — 6,061 entries across categories
- `tools.abi_analysis` — 3,599 ABI entries, **244 thiscall** recovered
- `tools.recomp --all --split 250` — 16 generated C files
  (`recomp_0000.c` … `recomp_0014.c`, `recomp_dispatch.c`,
  `recomp_stubs_unresolved.c`, `recomp_funcs.h`, `recomp_types.h`)
- MSVC configure + compile of the toolkit runtime libraries — OK

**Issue found & fixed:** the first run configured the toolkit as the CMake
source (a `-S .` + relative-path parsing quirk under pwsh), so only the
toolkit's libraries and test executables built and no game `.exe` appeared.
Fixed in the workflow with absolute `github.workspace` paths, an explicit
`--target fatalframe_native`, and a `CMAKE_HOME_DIRECTORY` assertion. The
second run confirmed the correct project is configured
(`CMAKE_HOME_DIRECTORY=.../engine`, `XBOXRECOMP_DIR=.../xboxrecomp`); a
PowerShell read-only `$home` variable broke the assertion script and was
renamed. Full disc extracted locally (1,856 files, 1,366 MB) into `game/`
next to the executable path.

**Notes for the crash loop**
- Single-player title, 103 kernel imports, no network dependency.
- XDK 4721-era kernel: simpler than the 5849 reference titles.
- FMVs are `.wmv` under `Media/movie/` (WMVDEC is a preloaded section here,
  not demand-loaded like Wreckless); the runtime auto-plays `.wmv` when the
  game opens the file.
- Textures/models are proprietary `.pkx`/`.xpr`/`.sgd`; the game's own code
  parses them — no host loader needed for boot.
- `DOLBY` section present: audio init may probe AC-3, watch for it.