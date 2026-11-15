#!/usr/bin/env python3
"""XBE inspection helpers for bring-up work (generic; no game code).

Subcommands:
  sections  XBE                 -- print the section table (VA, size, raw addr)
  peek      XBE VA [VA...]      -- print the initial (on-disk) dwords at VAs
  disasm    XBE VA END [START]  -- disassemble a VA range (32-bit x86)
  entry     XBE                 -- print the entry point and header facts

Works from the user's own XBE copy only. Nothing here is needed at run time;
it is for reading the original binary while diagnosing the port.

    py -3 xbe_tools.py sections  default.xbe
    py -3 xbe_tools.py peek      default.xbe 0xF6F48 0xF92FC
    py -3 xbe_tools.py disasm    default.xbe 0x000EEC60 0x000EED20
"""

import struct
import sys

SECTION_HEADER_FMT = '<IIIIIIIII'   # flags, va, vsize, raw, rawsize, name, refs, head, tail


def load_sections(path):
    data = open(path, 'rb').read()
    if data[0:4] != b'XBEH':
        raise SystemExit('not an XBE (magic %r)' % data[0:4])
    (hdr_size, image_size, xbe_hdr_size, _ts,
     _cert, sec_count, sec_hdrs_va, init_flags, entry_enc) = \
        struct.unpack_from('<IIIIIIIII', data, 0x104)
    entry = entry_enc ^ 0xA8FC57AB
    secs = []
    for i in range(sec_count):
        off = sec_hdrs_va + i * 0x38
        f, va, vsize, raddr, rsize, _n, _r, _h, _t = \
            struct.unpack_from(SECTION_HEADER_FMT, data, off)
        secs.append(dict(index=i, flags=f, va=va, vsize=vsize,
                         raw=raddr, rawsize=rsize))
    return data, secs, entry


def section_for(secs, va):
    for s in secs:
        if s['va'] <= va < s['va'] + s['vsize']:
            return s
    return None


def cmd_sections(path):
    _data, secs, entry = load_sections(path)
    print('entry point: 0x%08X' % entry)
    for s in secs:
        print('  sec%-2d flags=0x%08X va=0x%08X vsize=0x%-8X raw=0x%-8X rawsize=0x%X'
              % (s['index'], s['flags'], s['va'], s['vsize'], s['raw'], s['rawsize']))


def cmd_peek(path, vas):
    data, secs, _entry = load_sections(path)
    for va in vas:
        s = section_for(secs, va)
        if not s:
            print('0x%08X: no section covers it' % va)
            continue
        if s['raw'] == 0 or va - s['va'] >= s['rawsize']:
            print('0x%08X: BSS/uninitialized (sec%d covers it, no file bytes)'
                  % (va, s['index']))
            continue
        off = s['raw'] + (va - s['va'])
        if off + 4 > len(data):
            print('0x%08X: past EOF' % va)
            continue
        print('0x%08X: 0x%08X   (sec%d+0x%X)'
              % (va, struct.unpack_from('<I', data, off)[0], s['index'], va - s['va']))


def cmd_disasm(path, start, end):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    data, secs, _entry = load_sections(path)
    s = section_for(secs, start)
    if not s:
        raise SystemExit('no section covers 0x%08X' % start)
    base = s['raw'] - s['va']
    code = data[base + start: base + end]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for ins in md.disasm(code, start):
        print('  0x%08X: %-16s %s %s' % (ins.address, ins.bytes.hex(),
                                          ins.mnemonic, ins.op_str))


def cmd_entry(path):
    _data, secs, entry = load_sections(path)
    print('entry point: 0x%08X' % entry)
    for s in secs:
        if s['va'] <= entry < s['va'] + s['vsize']:
            print('  entry is in sec%d (va=0x%08X, vsize=0x%X)'
                  % (s['index'], s['va'], s['vsize']))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path, rest = argv[1], argv[2], argv[3:]
    if cmd == 'sections':
        cmd_sections(path)
    elif cmd == 'peek':
        cmd_peek(path, [int(v, 0) for v in rest])
    elif cmd == 'disasm':
        if len(rest) < 2:
            print('disasm needs VA END')
            return 2
        cmd_disasm(path, int(rest[0], 0), int(rest[1], 0))
    elif cmd == 'entry':
        cmd_entry(path)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))