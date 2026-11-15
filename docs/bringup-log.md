# Bring-up log

Running record of the Lethal Shot (Xbox) static recompilation bring-up.
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
aperture comment: the stance at the time was to bridge the D3D8 entry
point that owns the loop, not to model the GPU register file*). The garbage pointer makes the GET read
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

**Options at the time** (all since resolved — see the evening entry below;
the register model (option 2) is what was actually installed):
1. **Bridge the D3D8 entry points** (documented model): override the game's
   D3D entry (`sub_000EBFE0` from `sub_00011DD0`, then the device-create path)
   via `--manual-functions` + `recomp_lookup_manual` to return the toolkit's
   `xbox_Direct3DCreate8` / `IDirect3D8` vtable, so the game's D3D calls go
   through the toolkit's D3D11 layer and never touch NV2A MMIO. See
   `docs/technical/d3d-translation.md` in the toolkit (COM vtable hosting).
2. **Install the NV2A register model**: `nv2a_hook_init(g_xbox_mem_offset)`
   from the engine's `main.c` + protect the register aperture so the D3D8
   lib's reads get modelled values (boot id, PFIFO status, channel USER).
   Today `nv2a_hook_init` only protects the PVIDEO page and is not installed.

**Loop assets built**:
* `bringup-iterate.ps1` — local seed convergence (disasm + func_id + abi +
  recomp + unresolved-diff), ~4 min/round, no CI.
* `lethal-shot-build-farm/docs/HANDOFF.md` + `docs/PROMPT.md` — full handoff.

## 2026-11-14 — Project scaffolded, first cloud build green

**Setup**
- Confirmed disc: Xbox XDVDFS, `default.xbe` 1,773,568 bytes, title
  `0x54430004`, English retail build (no Shift-JIS menu strings in the XBE).
- Three repositories created on the owner's account:
  - `lethal-shot-engine` (public) — engine + runtime + docs, ships no
    game code
  - `lethal-shot-build-farm` (public) — CI build farm, no game code
  - a private input store (the title executable) — never named in public docs
  - a private artifact store (built binaries) — scaffolding, deleted when the port ships
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
`--target lethalshot_native`, and a `CMAKE_HOME_DIRECTORY` assertion. The
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

## 2026-11-15 (evening) — pushbuffer wait root cause + fix: PFIFO channel ack

**Status**: device create hangs in the same pushbuffer wait, root cause now
proven end-to-end against the on-disk XBE disassembly (not just probes).
Fix implemented in the engine (thread `engine_pfifo_ack_thread`); see commit
"PFIFO user-DMA ack".

**Corrections to the earlier entry** (both matter):
- `dev + 0x23BC` = `0x000F6F40 + 0x23BC` = **0x000F92FC** (the earlier
  "0x000F91FC" was an arithmetic slip) and its value is **0x00800000 — a
  valid channel pointer**, not garbage. The `0xE8500674` reading was the
  wrong address entirely.
- So the wait is *not* "GET read through a garbage pointer". Both pointers
  are fine. What never happens is the GPU half of the handshake: nothing
  advances the consumed position (GET).

**The mechanism** (disassembly of `0x000EEC78..0x000EECD6` in the D3D
section, file offset = VA − 0xE6B80 + 0xD7000):

```asm
  0x000EEC78: mov edx,[esi]              ; *esi saved onto the device
  0x000EEC7A: mov eax,[esp+8]            ; dev = 0x000F6F40
  0x000EEC7E: mov [eax+0x4fc],edx
  0x000EEC88: mov edx,[esp+0xc]          ; channel object (0x00800000 here)
  0x000EEC8C: mov [eax+0x23bc],edx        ; dev->channel = 0x00800000
  0x000EECA2: mov edx,[eax+0x24]         ; 0x80002000 (ring write cursor)
  0x000EECA5: and edx,0xfffffff          ; -> physical 0x2000
  0x000EECAB: inc edx
  0x000EECAC: mov [0x80000000],edx       ; *doorbell* kick (VA 0x80000000)
  0x000EECB2: wbinvd
  0x000EECB8: call 0x000EDA70            ; D3D flush (spins on the PFB
                                          ; kick bit at 0xFD100410, which
                                          ; the runtime's ack thread clears)
  0x000EECBD: eax=[esp+8]; ecx=[eax+0x23bc]; ecx=[ecx+0x44]   ; GET
  0x000EECCA: edx=[eax]                   ; PUT
  0x000EECCC: xor/test 0xfffffff ; jne 0x000EECD6 (busy delay 0xED750)
```

**Verified runtime state at the hang** (one probe run, all addresses):
`[0xF9B80]=0x000F6F40` (device global), `[0xF92FC]=0x00800000` (channel),
`[0x800040]=0x00002000` (channel PUT), `[0x800044]=0x00000000` (channel GET),
`[0xF6F40]=0x80002000` (device PUT), `[0x80000000]=0x00002001` (the kick that
was already delivered). `dev->[0x2C0] = 0xFD000000` is the NV2A aperture base
the same library uses for the PFB kick bit — the aperture path works; this
channel copy simply sits outside the aperture.

**Root cause**: the game's channel lives at guest VA `0x00800000` (PUT at
+0x40, GET at +0x44) and the kick is a write to the doorbell at VA
`0x80000000`. The runtime's NV2A ack thread models exactly this handshake —
PUT → GET at 0xFD800040/44 — for the aperture's USER window, but nothing
acknowledges the game's own channel object. GET stays 0, the wait spins.

**Fix** (`engine_pfifo_ack_thread`, started in step 5c): a 1 ms poll that,
whenever the channel shows the library waiting (PUT ahead of GET), reports
the submitted commands consumed (`GET = PUT`). Same modelling and same
honesty argument as the runtime's own ack thread: the push buffer is not
executed here (the toolkit's D3D11 layer draws), so "all consumed" is the
truthful answer. Field offsets are documented in `src/main.c` next to the
constants; all were cross-checked against this disassembly.

**Also confirmed on the way**:
- `sub_000EDAF6` (the small-submission path) sets `dev->[0x23BC] =
  0xF9B38` and `chan->[0x44] = chan->[0x40]` itself — the game knows how to
  ack its own channel when its `dev+8` bit 0x20 is set; here that bit is
  clear, so the main path (0x000EDA94) runs instead, whose GET-advance call
  `sub_000E7B90` is gated on `MEM32(0xF9B84)` — a global no instruction in the
  entire XBE ever writes (it is BSS, zero in the file), so on hardware it is
  presumably initialised by the GPU driver/ISR. The engine-level ack covers
  that gap without faking the global.
- The NV2A register model (previous commit) does answer the aperture reads:
  the library's PFB flush spin at 0x000EDAB4 now clears (the runtime ack
  thread's 0x100410 handling) and the boot proceeds to the pushbuffer wait.
- The tail-jump frame is *not* corrupted: `[esp]` in `sub_000EE943` is the
  device object exactly as the original expects (`dev+0x2C28 = 0x80001020`
  proves the prologue's `mov eax,[esp]` chain reads the saved slot), because
  the recomp restores esp to the frame level around ICALLs.

**Next iterations to expect** (each: engine tweak → CI ~19 min → run →
classify): the asset-loading path (Media/, .pkx/.xpr/.sgd), the vblank/PCRTC
interrupt the title may wait on before the first Present, and the audio init
(DSOUND section). The loop is unchanged: crash → `tools.seed_from_log` or
engine fix → rebuild.

## 2026-11-15 (late) — VRAM window fix: the boot reaches asset loading

**Fix**: `engine_nv2a_veh` now also routes faults in `0xF0000000`–`0xFCFFFFFF`
to the toolkit's `nv2a_hook_handle_vram` (it allocates zeroed pages on
demand — exactly what a hardware read gives a title before the first frame;
the real VRAM backing is future work).

**Result** (build `run-43765103996`, run on the owner's notebook with the
full disc): the pushbuffer wait at `0x000EECD6` no longer even runs, and the
boot reaches **file I/O**: 327 kernel calls, then a repeating cycle

```
RtlInitAnsiString (ord 289) -> NtCreateFile (190) -> NtReadFile (219) -> NtClose (187)
```

19 complete cycles, all from return addresses inside one function
(`0x000D84xx`–`0x000D8Axx`), then a crash reading guest VA `0xCA5DFE19`
with `esi = 0xCA5DFE19`, `eax = 0xFFFDA6EA`, `ecx = 0xADADADAD` — a pointer
chase through an uninitialised structure, i.e. the title is loading assets
from the disc and one of the reads produced (or exposed) a bad pointer.
Next step: identify the file the cycle was on when it crashed — the kernel
log names the path of every `NtCreateFile`, and the disc extraction in the
run directory is complete — then compare the read result against the file.

**Also fixed (CI-only, seen by the new smoke run)**: the NV2A install
trapped the aperture *before* installing the vectored handler, and the
runtime's ack thread ticks the register file continuously, so a fast
runner can take an unguarded fault in that window and die silently right
after the MMIO hook's init line (0 kernel calls). The handler is now
installed first, then the traps — ordering, not luck.

**Feedback channel for GitHub-only iteration**: `build.yml` now runs the
built exe on the runner for a bounded time (watchdog 25 s) and publishes
the log as the `smoke-run-<run-id>` artifact with a `SMOKE_SUMMARY` line
in the job log. Note the runner has only the XBE, not the disc, so a
smoke run reproduces the boot but not the file stage — for that, the
owner runs `tools/bringup/safe-run.ps1` locally and pastes the log.