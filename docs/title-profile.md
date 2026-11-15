# Lethal Shot (Xbox) — per-title profile

Everything below is **metadata about the XBE format** (headers, section layout,
import list). No game code or assets is included. Values are from
`py -3 -m tools.xbe_parser <xbe> --json` on a retail, user-owned copy.

## Identity

| Field | Value |
|---|---|
| Title | Lethal Shot (Xbox, original, 2002) |
| Debug path | `C:\<title>\Release\<title>.exe` |
| Title ID | `0x54430004` |
| Region | North America, Japan, Rest of World |
| Build | 2002-10-31, version 30724 |
| XDK | 1.0.4721 (all linked libs, early-2002 era) |
| Language | **English** (NA retail build — no Japanese menu strings in the XBE) |

## Memory layout

| Field | Value |
|---|---|
| Base address | `0x00010000` |
| Image size | `0x00395DC0` (3.59 MB) |
| Entry point | `0x000DAB5D` (raw `0xA8F1FCF6`), labeled `xbe_entry_point` |
| Kernel thunks | `0x00155FA0` |
| Stack commit | `0x16000` |
| Heap | reserve `0x100000` / commit `0x1000` |

## Sections (11)

| # | Name | VirtAddr | VirtSize | Notes |
|---|---|---|---|---|
| 0 | `.text` | `0x00011000` | `0x000D5B80` | game code, ~874 KB |
| 1 | `D3D` | `0x000E6B80` | `0x00013A0C` | statically linked D3D8 (XDK) |
| 2 | `D3DX` | `0x000FA5A0` | `0x00003980` | D3DX8 |
| 3 | `XGRPH` | `0x000FDF20` | `0x00001284` | XGRAPHC |
| 4 | `DSOUND` | `0x000FF1C0` | `0x0001E794` | DirectSound |
| 5 | `WMVDEC` | `0x0011D960` | `0x000328B0` | WMV/ASF decoder for FMVs |
| 6 | `XPP` | `0x00150220` | `0x00005D61` | TCP/IP + HTTP stack |
| 7 | `.rdata` | `0x00155FA0` | `0x00013268` | kernel thunk table |
| 8 | `.data` | `0x00169220` | `0x0023321C` | 2.25 MB, mostly runtime heap reservations |
| 9 | `DOLBY` | `0x0039C440` | `0x00007180` | Dolby AC3 |
| 10 | `$$XTIMAGE` | `0x003A35C0` | `0x00002800` | TLS + image header |

Linked libraries (all XDK `1.0.4721`): `XAPILIB`, `D3D8`, `D3DX8`, `XGRAPHC`,
`DSOUND`, `WMVDEC`, `XBOXKRNL`, `LIBCMT`. Kernel imports: **103**.

`XPP` and `DOLBY` are interesting for bring-up: single-player spare, the game
never touches the network stack, but Dolby output may be probed at audio init.

## Disc data layout (from XDVDFS listing)

```
default.xbe                    1773568
Media/
  dsstdfx.bin                   24824    (DSS/DS standard effects)
  font.xpr                      69632    (font texture atlas)
  3ddata/
    rshade.sgd                 (shader data)
    door/*.pkx                 (door scenes, ~180 files)
    man/mdl/*.xpr              (character models, 300-420 KB each)
  movie/scene*.wmv            (FMVs, 1-30 MB each, WMV/ASF via WMVDEC)
```

Assets are proprietary `.pkx` / `.xpr` / `.sgd` formats — the recompiled code
parses them with its own original routines, so no host-side asset loader is
needed for boot. The runtime only path-redirects the game's `D:\Media\...`
opens into the `game/` directory.

## Bring-up checklist (order matters)

1. CRT startup → `xbe_entry_point` at `0x000DAB5D`, heap init, globals
2. Hardware init → D3D8 device, DirectSound, `AvSetDisplayMode`
3. Video → `Media/movie/*.wmv` via WMVDEC (needs an ASF/WMV bitstream feed —
   the toolkit runtime may need a WMV path; watch for it at intro movies)
4. Menus → title screen, option menu, texture cache from `.pkx`/`.xpr`
5. Gameplay → Camera Obscura shots, ghost AI, save/load to memory unit
6. Audio → Dolby probe path may need stubbing

## Notes

* Single-threaded candidate: 103 kernel imports, no network dependency.
* Old XDK (4721): simpler kernel patterns, closer to the emulator reference
  implementations, than the 5849 titles.
* Region is NA/JP/ROW but the build is the English retail one.