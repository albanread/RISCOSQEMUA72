#!/usr/bin/env python3
"""Build a CMOS blob for RISC OS 5 on an emulated Raspberry Pi.

A Pi has no CMOS chip. The HAL takes its non-volatile settings from a blob the
firmware leaves in memory immediately after the OS image, reads it at `start`
before the MMU is on, and checks a version word; out of range and it blanks the
lot to 0xFF, which is what happens today and why the machine boots unconfigured.

Under emulation QEMU is the firmware, so we can supply that blob:

    qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \\
        -kernel RISCOS.IMG \\
        -device loader,file=cmos.bin,addr=0x510000,force-raw=on

The load address is not fixed: it is 0x10000 (where -kernel puts the image)
plus the OS image size from its header, plus 0x10000. Pass --rom and this
script prints the right address for your ROM.

Nothing here is transcribed by hand. The CMOS layout is read out of RISC OS's
own headers by simulating ObjAsm's storage map, and the defaults are parsed
from the kernel's DefaultCMOSTable, so a different RISC OS version produces a
correspondingly different blob. The layout is cross-checked against the address
comments in the header, and any mismatch is reported.

Why you would want one:

  --unplug N   stop ROM module chunk N initialising. Chunk 106 in RISC OS 5.30
               is EtherGENET, whose genet_attach leaves nicifp NULL when there
               is no Ethernet controller to find, after which a callback booked
               during its init dereferences it -- an abort on address 0x18 that
               no amount of device modelling can prevent.
  --language N which module the kernel starts. The stock default is 11, the
               Desktop; with no disc that fails and the kernel falls back to
               the supervisor prompt, so the prompt you normally see is a
               failure path rather than a choice. 1 asks for it deliberately.
"""
import argparse
import json
import re
import struct
import sys

CMOS_SIZE = 0x800          # 2048 bytes of settings
CHECKSUM_SEED = 1          # CMOSxseed, hdr/CMOS
VERSION_MIN, VERSION_MAX = 500, 600   # HAL s/Top checks 500 <= v < 600

# The kernel checksums the whole array except [CheckSumCMOS..0x100), per
# ChecksumSubBlock in Kernel/s/PMF/i2cutils.
SKIP_FROM, SKIP_TO = 0xEF, 0x100


def _num(tok, syms):
    tok = tok.strip()
    if tok.startswith('&'):
        return int(tok[1:], 16)
    m = re.fullmatch(r'(\d+)_([0-9a-fA-F]+)', tok)      # 2_1010, 4_3333
    if m:
        return int(m.group(2), int(m.group(1)))
    if re.fullmatch(r'-?\d+', tok):
        return int(tok)
    if tok in syms:
        return syms[tok]
    raise ValueError(f'unknown symbol {tok!r}')


def _eval(expr, syms):
    """Evaluate a simple ObjAsm expression."""
    e = expr.split(';')[0].strip()
    for op, py in ((':SHL:', '<<'), (':SHR:', '>>'), (':OR:', '|'),
                   (':AND:', '&'), (':EOR:', '^'), (':NOT:', '~')):
        e = e.replace(op, py)
    e = re.sub(r'&[0-9a-fA-F]+|\d+_[0-9a-fA-F]+|\w+',
               lambda m: str(_num(m.group(0), syms)), e)
    return eval(e, {'__builtins__': {}}, {})


def read_symbols(hdr_paths):
    """Resolve hdr/CMOS by simulating ObjAsm's ^ / # storage map.

    Returns (symbols, mismatches). Most declarations carry their address in a
    trailing comment, which gives us a free check on the whole map.
    """
    if isinstance(hdr_paths, str):
        hdr_paths = [hdr_paths]
    syms, checks = {}, []
    for hdr_path in hdr_paths:
        counter = 0                       # each header starts its own map
        for raw in open(hdr_path, encoding='utf-8', errors='replace'):
            raw = raw.rstrip('\n')
            m = re.match(r'^\s*\^\s*([^;]+)', raw)              # ^ <origin>
            if m:
                try:
                    counter = _eval(m.group(1), syms)
                except ValueError:
                    pass
                continue
            m = re.match(r'^(\w+)?\s*#\s*([^;]+?)\s*(?:;\s*(.*))?$', raw)
            if m and (m.group(1) or raw.lstrip().startswith('#')):
                name, size_s, comment = m.group(1), m.group(2), m.group(3) or ''
                try:
                    size = _eval(size_s, syms)
                except ValueError:
                    continue
                if name and name not in syms:
                    syms[name] = counter
                    cm = re.match(r'&([0-9A-Fa-f]+)', comment)
                    if cm:
                        checks.append((name, counter, int(cm.group(1), 16)))
                counter += size
                continue
            m = re.match(r'^(\w+)\s+\*\s+([^;]+)', raw)         # NAME * value
            if m:
                try:
                    syms[m.group(1)] = _eval(m.group(2), syms)
                except ValueError:
                    pass
    return syms, [(n, g, e) for n, g, e in checks if g != e]


def read_defaults(kernel_path, syms):
    """Parse DefaultCMOSTable out of Kernel/s/PMF/i2cutils."""
    lines = open(kernel_path, encoding='utf-8', errors='replace').read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith('DefaultCMOSTable'))
    entries, unresolved = {}, []
    for line in lines[start + 1:]:
        if re.match(r'^\w', line):                  # the next label ends it
            break
        m = re.match(r'\s+DCB\s+([^,;]+),\s*([^;]+?)\s*(?:;.*)?$', line)
        if not m:
            continue
        loc_s, val_s = m.group(1).strip(), m.group(2).strip()
        if loc_s.startswith('&FF'):                 # table terminator
            break
        try:
            entries[_eval(loc_s, syms) & 0xFF] = _eval(val_s, syms) & 0xFF
        except (ValueError, KeyError, SyntaxError) as ex:
            unresolved.append((loc_s, val_s, str(ex)))
    return entries, unresolved


def unplug_byte(chunk, syms):
    """Which CMOS byte and bit disable ROM module `chunk`.

    Kernel/s/ModHand indexes UnplugCMOSTable *backwards* from its end, so the
    arithmetic is easy to get wrong; this mirrors the kernel exactly.
    """
    # UnplugCMOSTable as laid out in the kernel: seventeen (name, offset)
    # bytes, the last of which sits past the UnplugCMOSTableEnd label and
    # serves the lowest chunks.
    table = [('Unplug17CMOS', 0), ('Unplug16CMOS', 0), ('Unplug15CMOS', 0),
             ('Unplug14CMOS', 0), ('Unplug13CMOS', 0), ('Unplug12CMOS', 0),
             ('Unplug11CMOS', 0), ('Unplug10CMOS', 0), ('Unplug9CMOS', 0),
             ('Unplug8CMOS', 0), ('Unplug7CMOS', 0),
             ('FrugalCMOS', 1), ('FrugalCMOS', 0),
             ('MosROMFrugalCMOS', 3), ('MosROMFrugalCMOS', 2),
             ('MosROMFrugalCMOS', 1), ('MosROMFrugalCMOS', 0)]
    first = syms.get('FirstUnpluggableModule', 8)
    if chunk < first:
        raise SystemExit(f'chunk {chunk} is below FirstUnpluggableModule ({first})')
    n = chunk - first
    byte_index, bit = n >> 3, n & 7
    offset = (len(table) - 1) - byte_index          # RSBCSS r1, r1, #End-Table
    if not 0 <= offset < len(table):
        raise SystemExit(f'chunk {chunk} is outside the unplug table')
    name, extra = table[offset]
    if name not in syms:
        raise SystemExit(f'{name} not found in hdr/CMOS')
    return syms[name] + extra, 1 << bit

def checksum(cmos):
    total = CHECKSUM_SEED + sum(cmos[0:SKIP_FROM]) + sum(cmos[SKIP_TO:CMOS_SIZE])
    return total & 0xFF


def load_address(rom_path):
    """0x10000 + OS image size + 0x10000, read from the image's own header."""
    data = open(rom_path, 'rb').read(0x10100)
    magic, _flags, image_size = struct.unpack_from('<III', data, 0x10000)
    if magic != 0x6D49534F:                          # 'OSIm'
        raise SystemExit(f'{rom_path}: no OS image header (magic {magic:#x})')
    return 0x10000 + image_size + 0x10000


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--output', default='cmos.bin')
    ap.add_argument('--riscos-src', required=True,
                    help='path to .../BCM2835/RiscOS')
    ap.add_argument('--rom', help='ROM image, to compute the load address')
    ap.add_argument('--unplug', type=int, action='append', default=[],
                    metavar='CHUNK', help='disable ROM module chunk (repeatable)')
    ap.add_argument('--language', type=int,
                    help='module the kernel starts (stock default 11 = Desktop)')
    ap.add_argument('--filesystem', type=int, metavar='N',
                    help='filing system to boot from, by number: 192 = SDFS, '
                         '23 = RamFS, 8 = ADFS (the stock default). This is '
                         'what *Configure FileSystem writes.')
    ap.add_argument('--set', action='append', default=[], metavar='LOC=VAL',
                    help='set any CMOS byte. LOC may be a name from hdr/CMOS '
                         '(e.g. LanguageCMOS) or a number; repeatable.')
    ap.add_argument('--base', metavar='FILE',
                    help='start from an existing blob instead of the kernel '
                         'defaults: the CMOS file from a Pi boot partition '
                         '(2048 bytes, optionally followed by its version word)')
    ap.add_argument('--version', type=int, default=530)
    args = ap.parse_args()

    if not VERSION_MIN <= args.version < VERSION_MAX:
        raise SystemExit(f'version must be {VERSION_MIN}..{VERSION_MAX - 1}; '
                         'the HAL blanks the blob otherwise')

    hdr = f'{args.riscos_src}/Sources/Programmer/HdrSrc/hdr/CMOS'
    knl = f'{args.riscos_src}/Sources/Kernel/s/PMF/i2cutils'

    fsn = f'{args.riscos_src}/Sources/Programmer/HdrSrc/hdr/FSNumbers'
    syms, mismatches = read_symbols([hdr, fsn])
    print(f'{len(syms)} symbols from hdr/CMOS', file=sys.stderr)
    for name, got, want in mismatches:
        print(f'  warning: {name} maps to &{got:02X}, its comment says &{want:02X}',
              file=sys.stderr)

    defaults, unresolved = read_defaults(knl, syms)
    print(f'{len(defaults)} defaults from DefaultCMOSTable', file=sys.stderr)
    for loc, val, why in unresolved:
        print(f'  warning: skipped {loc} = {val} ({why})', file=sys.stderr)

    if args.base:
        base = open(args.base, 'rb').read()
        if len(base) < CMOS_SIZE:
            raise SystemExit(f'{args.base}: {len(base)} bytes, want {CMOS_SIZE}')
        cmos = bytearray(base[:CMOS_SIZE])
        print(f'starting from {args.base}', file=sys.stderr)
    else:
        cmos = bytearray(CMOS_SIZE)
        for loc, val in defaults.items():
            cmos[loc] = val
        # The HAL leaves everything from the checksum byte up blank, and the
        # kernel never resets it, so match that rather than inventing zeros.
        for i in range(SKIP_FROM + 1, CMOS_SIZE):
            cmos[i] = 0xFF

    for chunk in args.unplug:
        loc, bit = unplug_byte(chunk, syms)
        cmos[loc] |= bit
        print(f'unplugging chunk {chunk}: &{loc:02X} |= {bit:#04x}', file=sys.stderr)

    if args.language is not None:
        cmos[syms['LanguageCMOS']] = args.language
        print(f'language module = {args.language}', file=sys.stderr)

    if args.filesystem is not None:
        cmos[syms['FileLangCMOS']] = args.filesystem
        print(f'boot filing system = {args.filesystem} '
              f'(&{syms["FileLangCMOS"]:02X})', file=sys.stderr)

    for spec in args.set:
        loc_s, _, val_s = spec.partition('=')
        if not val_s:
            raise SystemExit(f'--set wants LOC=VAL, got {spec!r}')
        loc = syms[loc_s] if loc_s in syms else _eval(loc_s, syms)
        val = _eval(val_s, syms) & 0xFF
        if not 0 <= loc < CMOS_SIZE:
            raise SystemExit(f'--set {spec}: &{loc:X} is outside the CMOS')
        cmos[loc] = val
        print(f'set &{loc:02X} = &{val:02X}   ({loc_s})', file=sys.stderr)

    cmos[SKIP_FROM] = 0
    cmos[SKIP_FROM] = checksum(cmos)
    blob = bytes(cmos) + struct.pack('<I', args.version)
    open(args.output, 'wb').write(blob)

    print(f'wrote {args.output}: {len(blob)} bytes, '
          f'checksum &{cmos[SKIP_FROM]:02X}, version {args.version}', file=sys.stderr)
    if args.rom:
        print(f'load it at 0x{load_address(args.rom):08x}', file=sys.stderr)


if __name__ == '__main__':
    main()
