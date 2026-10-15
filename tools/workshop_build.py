#!/usr/bin/env python3
"""Build a hundekraft workshop firmware on the command line (the same result as the web workshop).

  python3 tools/workshop_build.py WORKSHOP_DIR VARIANT op1_246.op1 OUT.op1

WORKSHOP_DIR is a checkout of https://github.com/Hundekraft/op1-workshop (its published release: manifests and
patch payloads, no firmware). VARIANT is the selection id, e.g. 7ff = every synth, effect and utility with 300
presets (see the workshop's catalog.json and config_id in its runtime.py). The steps are the workshop's own: check
the original R246 image, apply the base and the variant manifests (sha256-checked byte replacements and insertions,
added files), check the archive's sha256 against the variant's, keep the bootloader, pack (tar, LZMA, CRC-32).
Then add roll and drive with synth_sdk/make_overlay.py.
"""
import binascii, copy, hashlib, io, json, lzma, os, struct, sys, tarfile

sha = lambda b: hashlib.sha256(b).hexdigest()


def checked(data, want, what):
    if sha(data) != want:
        raise SystemExit(f'{what}: sha256 mismatch')
    return data


def unpack(op1):
    if struct.unpack('<I', op1[:4])[0] != binascii.crc32(op1[4:]):
        raise SystemExit('CRC-32 mismatch')
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(op1[4:], format=lzma.FORMAT_ALONE))) as t:
        return [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]


def pack_tar(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as t:
        for info, data in members:
            info = copy.copy(info)
            if data is not None:
                info.size = len(data)
            t.addfile(info, io.BytesIO(data) if data is not None else None)
    return buf.getvalue()


def apply(members, man, folder):
    members = list(members)
    idx = {m.name: i for i, (m, _) in enumerate(members)}
    for e in man['patched_files']:
        info, data = members[idx[e['path']]]
        buf = bytearray(checked(data, e['orig_sha256'], e['path']))
        for op in e['ops']:
            o = op['offset']
            if op['op'] == 'replace':
                old, new = bytes.fromhex(op['old']), bytes.fromhex(op['new'])
                if len(old) != len(new) or buf[o:o + len(old)] != old:
                    raise SystemExit(f'{e["path"]}: unexpected bytes at {o:#x}')
                buf[o:o + len(old)] = new
            elif op['op'] == 'insert':
                blob = checked(open(os.path.join(folder, op['blob']), 'rb').read(), op['sha256'], op['blob'])
                if len(blob) != op['size']:
                    raise SystemExit(f'{op["blob"]}: size mismatch')
                buf[o:o] = blob
            elif op['op'] == 'truncate':
                del buf[o:]
            else:
                raise SystemExit('unknown operation ' + op['op'])
        members[idx[e['path']]] = (info, checked(bytes(buf), e['result_sha256'], e['path']))
    for e in man['added_files']:
        data = members[idx[e['copy_of']]][1] if 'copy_of' in e else open(os.path.join(folder, e['blob']), 'rb').read()
        checked(data, e['sha256'], e['path'])
        info = copy.copy(members[idx[e['template']]][0])
        info.name = e['path']
        idx[e['path']] = len(members)
        members.append((info, data))
    checked(pack_tar(members), man['result']['tar_sha256'], 'archive')
    return members


def main(work, variant, src, out):
    op1 = open(src, 'rb').read()
    base = json.load(open(os.path.join(work, 'base', 'manifest.json')))
    var = json.load(open(os.path.join(work, 'variants', variant, 'manifest.json')))
    checked(op1, base['original']['sha256'], 'not the original R246 image')
    orig = unpack(op1)
    members = apply(apply(orig, base, os.path.join(work, 'base')), var, os.path.join(work, 'variants', variant))
    boot = lambda ms: [d for m, d in ms if m.name == 'te-boot.ldr'][0]
    if boot(members) != boot(orig):
        raise SystemExit('bootloader changed')
    comp = lzma.compress(pack_tar(members), format=lzma.FORMAT_ALONE, filters=[
        {'id': lzma.FILTER_LZMA1, 'preset': 9, 'lc': 3, 'lp': 1, 'pb': 2, 'dict_size': 2 ** 23}])
    img = struct.pack('<I', binascii.crc32(comp)) + comp
    open(out, 'wb').write(img)
    print(f'{var["filename"]} ({json.dumps(var["config"])}) -> {out}, sha256 {sha(img)}')


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
