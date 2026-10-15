#!/usr/bin/env python3
"""Add roll and drive to another firmware image built on op1r (e.g. a hundekraft build), without its sources.

  ./make_overlay.py BASE.op1 [-o OUT.op1] [--native]          -> ../<base>+roll+drive.op1

BASE.op1 is your own copy of that image (never stored in this repository). Steps:
  1. read the base image and find, at every site roll and drive hook, what the base has there: its own hook
     (CALL / JUMP.L target, vtable word, the ScreenId fx case stub) or the stock code; each check below must pass;
  2. compile overlay/overlay.c + seq/roll.c + plugins/drive/drive.c (bfin-elf-gcc), linked into a free gap of the
     base image, with the base's hooks as the next links (--defsym next_*=...);
  3. patch the sites to the overlay, insert its LDR block, add the seq_types row "roll" and the fx_types row
     "drive" to op1_factory.db, add roll.svg / drive.svg, add "+ roll + drive" to the boot text when it fits;
  4. pack the .op1 (tar, LZMA, CRC-32), the bootloader untouched.

Nothing of the base's own code is written here: a new release of the base works as long as the checks pass.
Do not distribute an image built with this without the base's authors' consent.
"""
import argparse, binascii, copy, hashlib, io, json, lzma, os, sqlite3, struct, subprocess, sys, tarfile, tempfile
import make_firmware as M
import plugins

HERE = os.path.dirname(os.path.abspath(__file__))
OVL = os.path.join(HERE, 'overlay')
FX_MARK = 0x5340                                # overlay/overlay.c DV_MARK
CODE_BASE, CODE_END = 0x01E00000, 0x01F00000   # op1r's block, end of the free window (te-boot above)
CLOCK_RESET_ORIG = bytes.fromhex('09e17c1849e18c010060089348b088b01000')   # 0x01A5E7D0, R246
FX_CASE_TAIL = bytes.fromhex('78ac01e800005000')   # P0 = [FP + 4]; UNLINK; JUMP (P0)  (after the CALL)
BOOT_TEXT_IMM = 0x01A2FCB8                       # R0.L / R0.H = boot text address (stock 0x018F7358)
# (site, kind, overlay symbol, next symbol): kind 'call' / 'jump' = the base must have a CALL / JUMP.L there (its
# hook), 'stock_call' = the stock CALL (target checked), 'stock_entry' = untouched stock bytes, 'word' = vtable word
SITES = [
    (0x01A42A2E, 'call', 'ov_seq_from_name', 'next_seq_from_name'),      # SeqIds::from_name (set_sequencer)
    (0x01A42BDC, 'call', 'ov_seq_from_name', 'next_seq_from_name'),      # .. (load, synth)
    (0x01A42CE0, 'call', 'ov_seq_from_name', 'next_seq_from_name'),      # .. (load, drum)
    (0x01A422EC, 'jump', 'ov_seq_name', 'next_seq_name'),                # SeqIds::name entry
    (0x01A0D1B0, 'stock_call', 'rl_player_get', None),                   # create_player case 5: arpeggio
    (0x01A5E7D0, 'stock_entry', 'rl_clock_reset', None),                 # sequencer clock reset
    (0x01A3BD58, 'call', 'ov_screens_ctor', 'next_screens_ctor'),        # screen construction
    (0x019F07D2, 'call', 'ov_screens_load', 'next_screens_load'),        # boot: load every screen
    (0x019C2D3E, 'call', 'ov_key', 'next_key'),                          # KeyHandler: key event
    (0x019C2E94, 'call', 'ov_knob', 'next_knob'),                        # KeyHandler: encoder step
    (0x019AAD8E, 'jump', 'ov_fx_name_eq', 'next_fx_name_eq'),            # fx_types name compare
    (0x01910CA8, 'word', 'ov_fx_mono', 'next_fx_mono'),                  # filter vtable: render
    (0x01910CB0, 'word', 'ov_fx_mono_pos', 'next_fx_mono_pos'),          # .. set_position
    (0x01910CC8, 'word', 'ov_fx_stereo', 'next_fx_stereo'),              # stereo (master) filter: render
    (0x01910CD0, 'word', 'ov_fx_stereo_pos', 'next_fx_stereo_pos'),      # .. set_position
    (0x018F7FF8, 'fxcase', 'ov_fx_case', 'next_fx_screen'),              # ScreenId fx case 5
]
STOCK_TARGETS = {0x01A0D1B0: 0x01A46400}


def sh(cmd):
    print('$', cmd); subprocess.run(cmd, shell=True, check=True)


def unpack(path):
    raw = open(path, 'rb').read()
    if struct.unpack('<I', raw[:4])[0] != binascii.crc32(raw[4:]):
        raise SystemExit(f'{path}: CRC-32 mismatch')
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(raw[4:], format=lzma.FORMAT_ALONE))) as t:
        return [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]


def pack(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as t:
        for info, data in members:
            info = copy.copy(info)
            if data is not None:
                info.size = len(data)
            t.addfile(info, io.BytesIO(data) if data is not None else None)
    comp = lzma.compress(buf.getvalue(), format=lzma.FORMAT_ALONE, filters=[
        {'id': lzma.FILTER_LZMA1, 'preset': 9, 'lc': 3, 'lp': 1, 'pb': 2, 'dict_size': 2 ** 23}])  # as op1repacker
    out = struct.pack('<I', binascii.crc32(comp)) + comp
    assert len(out) < 15_000_000, 'image too large for the updater'
    return out


def discover(ldr):
    """-> {next symbol: address} from the base image, after checking every site"""
    nxt = {}
    for site, kind, ours, n in SITES:
        b = M.ldr_read(ldr, site, 4)
        if kind in ('call', 'jump'):
            t = M.branch_at(site, b)
            if not t or t[1] != (kind == 'call'):
                raise SystemExit(f'{site:#x}: expected a {kind.upper()} of the base image, found {b.hex()}')
            nxt.setdefault(n, t[0])
            if nxt[n] != t[0]:
                raise SystemExit(f'{site:#x}: {n} differs between its sites ({nxt[n]:#x} / {t[0]:#x})')
        elif kind == 'stock_call':
            t = M.branch_at(site, b)
            if not t or not t[1] or t[0] != STOCK_TARGETS[site]:
                raise SystemExit(f'{site:#x}: the base changed it (expected the stock CALL): someone else hooks it')
        elif kind == 'stock_entry':
            if M.ldr_read(ldr, site, len(CLOCK_RESET_ORIG)) != CLOCK_RESET_ORIG:
                raise SystemExit(f'{site:#x}: the base changed the stock clock reset: someone else hooks it')
        elif kind == 'word':
            nxt[n] = struct.unpack('<I', b)[0]
        elif kind == 'fxcase':
            stub = struct.unpack('<I', b)[0]
            code = M.ldr_read(ldr, stub, 12)
            t = M.branch_at(stub, code)
            if not t or not t[1] or code[4:12] != FX_CASE_TAIL:
                raise SystemExit(f'{site:#x}: ScreenId fx case 5 -> {stub:#x} is not the expected stub ({code.hex()})')
            nxt[n] = t[0]
    return nxt


def free_gap(ldr, size):
    """first gap after op1r's block (0x01E00000) in the free window that holds `size` bytes"""
    used = sorted((a, a + n) for o, c, a, n, g in M.ldr_blocks(ldr) if n and CODE_BASE <= a < CODE_END)
    if not used or used[0][0] != CODE_BASE:
        raise SystemExit('no op1r block at 0x01E00000 in the base image')
    start = (used[0][1] + 0xFF) & ~0xFF
    for a, e in used[1:] + [(CODE_END, CODE_END)]:
        if a >= start + size:
            return start, a
        start = max(start, (e + 0xFF) & ~0xFF)
    raise SystemExit(f'no free gap of {size} bytes in {CODE_BASE:#x}-{CODE_END:#x}')


def build(nxt, base_addr, native):
    """compile + link at base_addr -> (blob, symbols)"""
    os.makedirs(os.path.join(HERE, 'build', 'overlay'), exist_ok=True)
    drive = [p for p in plugins.load()[1] if p['name'] == 'drive'][0]
    srcs = ['overlay/overlay.c', 'seq/roll.c', drive['source']] + drive.get('extra_sources', [])
    objs, cmds = [], []
    for s in srcs:
        o = 'build/overlay/' + s.replace('/', '_').replace('.c', '.o'); objs.append(o)
        cmds.append(f'bfin-elf-gcc -O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common -Wall -I. -c {s} -o {o}')
    defs = ' '.join(f'--defsym=_{k}={v:#x}' for k, v in sorted(nxt.items()))
    cmds.append(f'bfin-elf-ld -T overlay/overlay.ld --defsym=OVL_BASE={base_addr:#x} {defs} {" ".join(objs)} '
                f'$(bfin-elf-gcc -print-libgcc-file-name) -o build/overlay/overlay.elf')
    cmds.append('bfin-elf-objcopy -O binary -j .blob build/overlay/overlay.elf build/overlay/overlay.bin')
    cmds.append('bfin-elf-nm build/overlay/overlay.elf > build/overlay/overlay.sym')
    cmds.append('bfin-elf-objdump -d build/overlay/overlay.elf > build/overlay/overlay.lst')
    if native:
        sh(f'cd "{HERE}" && {" && ".join(cmds)}')
    else:
        sh(f'docker run --rm -u {os.getuid()}:{os.getgid()} -v "{HERE}":/work bfin-elf-toolchain sh -c \'{" && ".join(cmds)}\'')
    syms = {}
    for line in open(os.path.join(HERE, 'build', 'overlay', 'overlay.sym')):
        a, t, n = line.split(); syms[n.lstrip('_')] = int(a, 16)
    blob = open(os.path.join(HERE, 'build', 'overlay', 'overlay.bin'), 'rb').read()
    return blob + b'\0' * (-len(blob) % 4), syms


def boot_text(ldr, d):
    """' + roll + drive' at the end of the base's boot text, in place: the last line loses as many leading spaces, so
    the text keeps its length and its right edge. -> the new text, or None (no room)"""
    addr = struct.unpack('<H', M.ldr_read(ldr, BOOT_TEXT_IMM + 2, 2))[0] | \
        struct.unpack('<H', M.ldr_read(ldr, BOOT_TEXT_IMM + 6, 2))[0] << 16
    blk = [(a, n) for o, c, a, n, g in M.ldr_blocks(ldr) if not c & 0x1500 and n and a <= addr < a + n]
    if not blk:
        return None
    text = M.ldr_read(ldr, addr, blk[0][0] + blk[0][1] - addr).split(b'\0')[0]
    head, nl, last = text.rpartition(b'\n')
    tag = b' + roll + drive'
    if len(last) - len(last.lstrip(b' ')) < len(tag):
        return None
    new = head + nl + last[len(tag):] + tag
    M.ldr_poke(d, addr, new, text)
    return new.decode(errors='replace').strip()


def main(base, out, native):
    members = unpack(base)
    files = {m.name: i for i, (m, _) in enumerate(members)}
    ldr = members[files['OP1_vdk.ldr']][1]
    print(f'base {base}: sha256 {hashlib.sha256(open(base, "rb").read()).hexdigest()}')
    nxt = discover(ldr)
    for k, v in sorted(nxt.items()):
        print(f'  {k:20s} {v:#010x}')
    # link once at a provisional address to know the size, then at the gap that holds it
    blob, syms = build(nxt, 0x01E30000, native)
    start, limit = free_gap(ldr, len(blob))
    blob, syms = build(nxt, start, native)
    assert start + len(blob) <= limit, (hex(start), len(blob), hex(limit))
    print(f'overlay {len(blob)} bytes at {start:#x} (free up to {limit:#x})')
    d = bytearray(ldr)
    for site, kind, ours, n in SITES:
        old = M.ldr_read(ldr, site, 4)
        if kind in ('call', 'stock_call'):
            M.ldr_poke(d, site, M.branch(site, syms[ours], True), old)
        elif kind in ('jump', 'stock_entry'):
            M.ldr_poke(d, site, M.branch(site, syms[ours], False), old)
        else:
            M.ldr_poke(d, site, struct.pack('<I', syms[ours]), old)
    text = boot_text(ldr, d)
    fin = [o for o, c, a, n, g in M.ldr_blocks(d) if c & 0x8000][0]
    d[fin:fin] = M.ldr_hdr(0x0006, start, len(blob), 0) + blob
    c0, a0, n0, g0 = struct.unpack_from('<IIII', d, 0)
    d[0:16] = M.ldr_hdr(c0 & 0xFFFF, a0, n0, g0 + 16 + len(blob))
    members[files['OP1_vdk.ldr']] = (members[files['OP1_vdk.ldr']][0], bytes(d))
    # database rows: next free ids, drive's defaults as make_firmware.py computes them (selector FX_MARK)
    drive = [p for p in plugins.load()[1] if p['name'] == 'drive'][0]
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, 'f.db')
        open(p, 'wb').write(members[files['content/op1_factory.db']][1])
        c = sqlite3.connect(p)
        if c.execute("SELECT 1 FROM seq_types WHERE type='roll'").fetchone() or \
           c.execute("SELECT 1 FROM fx_types WHERE type='drive'").fetchone():
            raise SystemExit('the base already has roll or drive')
        sid = c.execute('SELECT MAX(id) FROM seq_types').fetchone()[0] + 1
        c.execute('INSERT INTO seq_types VALUES (?,?)', (sid, 'roll'))
        k = list(drive['defaults']) + [0] * 4
        raw = [lo + (min(max(v, 0), 32767) * (hi - lo)) // 32767 for v, (lo, hi) in zip(k[:4], M.FX_RANGES)]
        p2 = list(drive.get('page2_defaults', [])) + [0] * 3
        fid = c.execute('SELECT MAX(id) FROM fx_types').fetchone()[0] + 1
        params = json.dumps(raw + [FX_MARK] + [min(max(int(v), 0), 32767) for v in p2[:3]])
        c.execute('INSERT INTO fx_types VALUES (?,?,?)', (fid, 'drive', params))
        c.commit()
        assert c.execute('PRAGMA integrity_check').fetchone() == ('ok',)
        c.close()
        members[files['content/op1_factory.db']] = (members[files['content/op1_factory.db']][0], open(p, 'rb').read())
    print(f'  seq_types ({sid}, roll); fx_types ({fid}, drive, {params})')
    tmpl = [m for m, _ in members if m.name.startswith('content/display/') and m.isfile()][0]
    for name, src in (('roll.svg', os.path.join(OVL, 'roll.svg')), ('drive.svg', os.path.join(HERE, drive['screen']['svg']))):
        path = 'content/display/' + name
        if path in files:
            raise SystemExit(f'{path} already in the base image')
        info = copy.copy(tmpl); info.name = path
        members.append((info, open(src, 'rb').read()))
    orig_boot = [dt for m, dt in unpack(base) if m.name == 'te-boot.ldr'][0]
    assert [dt for m, dt in members if m.name == 'te-boot.ldr'][0] == orig_boot
    img = pack(members)
    open(out, 'wb').write(img)
    print(f'boot text: {text!r}' if text else 'boot text unchanged (no room)')
    print(f'-> {out} ({len(img)} bytes, sha256 {hashlib.sha256(img).hexdigest()})')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', help='firmware image built on op1r (your own copy)')
    ap.add_argument('-o', '--out')
    ap.add_argument('--native', action='store_true', help='bfin-elf tools from PATH instead of Docker')
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(HERE), os.path.basename(a.base)[:-4] + '+roll+drive.op1')
    main(a.base, out, a.native)
