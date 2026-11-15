#!/usr/bin/env python3
"""Analyse the generated recompilation C (generic; no game code).

The generated C (src/recomp/gen/recomp_XXXX.c on this project) is the
translation of the title's own code, so it is the ground truth for
behaviour questions the runtime log cannot answer alone: which function
writes a field, what a loop compares, where a pointer comes from.

    py -3 analyze_gen.py body   <gen-dir> sub_000EE943      # print one function
    py -3 analyze_gen.py find   <gen-dir> '0x23BC'          # every access to a field
    py -3 analyze_gen.py calls  <gen-dir> sub_000EE943      # its direct callees
    py -3 analyze_gen.py whom   <gen-dir> 0x000EECD6        # function containing a VA

Example: the D3D pushbuffer wait at 0x000EECD6 lives in sub_000EE943; the
channel pointer it reads is stored at dev+0x23BC, and 'find 0x23BC' shows
every reader and writer of that field.
"""

import glob
import os
import re
import sys


def load(gen_dir):
    funcs = {}
    for path in sorted(glob.glob(os.path.join(gen_dir, 'recomp_00*.c'))):
        src = open(path, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'^void (sub_[0-9A-F]{8})\(void\)\n\{', src, re.M):
            name = m.group(1)
            end = src.find('\n}\n', m.end())
            if end < 0:
                end = len(src)
            funcs[name] = (path, src[m.start():end].splitlines())
    return funcs


def cmd_body(funcs, name):
    path, lines = funcs.get(name, (None, None))
    if not lines:
        print('no such function: %s' % name)
        return 1
    print('# %s (%s, %d lines)' % (name, os.path.basename(path), len(lines)))
    for i, ln in enumerate(lines):
        print('%4d: %s' % (i, ln.rstrip()))
    return 0


def cmd_find(funcs, needle):
    for name, (_path, lines) in funcs.items():
        for ln in lines:
            if needle in ln:
                print('%s: %s' % (name, ln.strip()))
    return 0


def cmd_calls(funcs, name):
    path, lines = funcs.get(name, (None, None))
    if not lines:
        print('no such function: %s' % name)
        return 1
    seen = []
    for ln in lines:
        for m in re.finditer(r'RECOMP_ABI_CALL\(0x([0-9A-F]{8})u', ln):
            t = '0x' + m.group(1)
            if t not in seen:
                seen.append(t)
        for m in re.finditer(r'\b(sub_[0-9A-F]{8})\(\);', ln):
            t = m.group(1)
            if t not in seen:
                seen.append(t)
    print('%s calls: %s' % (name, ', '.join(seen) or '(none found)'))
    return 0


def cmd_whom(funcs, va):
    va = va.lower().replace('0x', '').zfill(8)
    for name, (path, lines) in funcs.items():
        for ln in lines:
            if va in ln.lower():
                print('%s (%s): %s' % (name, os.path.basename(path), ln.strip()))
                break
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, gen_dir, rest = argv[1], argv[2], argv[3:]
    funcs = load(gen_dir)
    if cmd == 'body':
        return cmd_body(funcs, rest[0])
    if cmd == 'find':
        return cmd_find(funcs, rest[0])
    if cmd == 'calls':
        return cmd_calls(funcs, rest[0])
    if cmd == 'whom':
        return cmd_whom(funcs, rest[0])
    print(__doc__)
    return 2


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))