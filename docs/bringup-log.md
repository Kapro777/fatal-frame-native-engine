# Bring-up log

Running record of the Fatal Frame (Xbox) static recompilation bring-up.
Newest entries at the bottom.

## 2026-11-15 — D3D device create fixed (seeds + entry-stubs); next blocker: NV2A channel

**Fixed**: the D3D device create failure from the first run. Root cause: the
XDK D3D8 lib's code after the device-context allocation is entered through a
**tail-jump to 0x000EE943** — an entry point in the middle of its own body
(`sub_000EE900`, size 67 bytes). The static detector never emitted a function
at 0xEE943, so the recomp emitted a call to a non-existent symbol absorbed by
`recomp_stubs_unresolved.c` as an empty stub (`g_esp += 4; return`) — the entire
rest of device creation never ran, and the caller read the alloc pointer
(0x80001000) as a negative HRESULT.

**Fix**: (a) seed every unresolved call target — found by diffing
called-vs-defined `sub_XXXXXXXX` symbols in the locally generated C
(276 → converged over 3 rounds to 808 seeds); (b) the recomp flag
`--entry-stubs`, which turns unresolved targets that lie inside known
functions into copies entered at the target address. With both, the generated
C grew 3,633 → 4,821 functions, the 512 KB allocation (`sub_000ED7E0`) runs,
`MmClaimGpuInstanceMemory` + the interrupt setup (KeInitializeDpc,
KeConnectInterrupt, HalRegisterShutdownNotification) + HalReadWritePCISpace
all run.

**Next blocker (fully diagnosed)**: the game hangs in the XDK D3D8 lib's
pushbuffer DMA wait at `0x000EECD6` (inside `sub_000EE943`):

```asm
L: call 0xED750                  ; 400-iteration busy delay
   eax = [esp+8]                 ; dev (the D3D8 object, 0x000F6F40)
   ecx = [eax+0x23BC]            ; dev->channel_ptr
   ecx = [ecx+0x44]              ; DMA_GET from the channel's USER area
   edx = [eax]                   ; DMA_PUT (0x80002000 at the hang)
   test (edx ^ ecx), 0xFFFFFFF   ; wait until GPU consumed the ring
   jne L
```

`dev->channel_ptr` (`[0x000F91FC]`) is **garbage (0xE8500674)** because the
XDK D3D8 lib's NV2A channel/PFIFO initialization reads/writes hardware
registers through the NV2A aperture — which the runtime backs as plain zeroed
RAM with "no register semantics" (see `xbox_memory_layout.c`, the NV2A
aperture comment: *"the fix is to bridge the D3D8 entry point that owns the
loop, not to start emulating NV2A"*). The garbage pointer makes the GET read
return 0 forever → the wait never completes → the watchdog fires (guest
esp=0x00F7FD94, edx=0x80002000).

**Probe evidence** (runtime bring-up probes, not fixes):
* `RECOMP_PEEK="0xF91FC,0xF6F40"` → `[0xF91FC]=0xE8500674` (channel ptr),
  `[0xF6F40]=0x80002000` (PUT) at the hang.
* `RECOMP_POKE="0xF6F40:0"` (force PUT=0 so PUT==GET) → the wait passes and
  the game runs further, then faults chasing a corrupted pointer
  (`[CRASH] ... Xbox VA of fault: 0xF71182A3`). The probe state is
  inconsistent downstream by construction — see the POKE comment in the
  toolkit.

**Options for the next engineer** (in order of preference):
1. **Bridge the D3D8 entry points** (documented model): override the game's
   D3D entry (`sub_000EBFE0` from `sub_00011DD0`, then the device-create path)
   via `--manual-functions` + `recomp_lookup_manual` to return the toolkit's
   `xbox_Direct3DCreate8` / `IDirect3D8` vtable, so the game's D3D calls go
   through the toolkit's D3D11 layer and never touch NV2A MMIO. See
   `docs/technical/d3d-translation.md` in the toolkit (COM vtable emulation).
2. **Install the NV2A register model**: `nv2a_hook_init(g_xbox_mem_offset)`
   from the engine's `main.c` + protect the register aperture so the D3D8
   lib's reads get modelled values (boot id, PFIFO status, channel USER).
   Today `nv2a_hook_init` only protects the PVIDEO page and is not installed.
3. **xemu comparison as ground truth** (xemu downloaded at
   `C:\Users\USER\fatal\xemu\`) to confirm the boot chain on real hardware.

**Loop assets built**:
* `bringup-iterate.ps1` — local seed convergence (disasm + func_id + abi +
  recomp + unresolved-diff), ~4 min/round, no CI.
* `fatal-frame-xbox-recomp/docs/HANDOFF.md` + `docs/PROMPT.md` — full handoff.

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