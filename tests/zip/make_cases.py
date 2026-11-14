# Round 29 phase 29B: writes the zip reader's test cases (tests/zip/*.zip + *.expected) that
# WinDockTests runs (tests/zip/ZipTests.cpp). Run it again after changing a case:
#   python -I tests/zip/make_cases.py
# The archives are built by hand (a minimal writer below) so that each one can carry exactly the
# flaw it tests; the deflate data comes from zlib with every level and strategy, so the decoder
# meets stored, fixed and dynamic blocks, long and overlapping matches.
# An .expected file is "ok" and one line per file entry "<name> <size> <crc32 hex>", or
# "error <text the error must contain>".
import os, random, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))

def deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    return c.compress(data) + c.flush()

def archive(entries, eocd_entries=None):
    """entries: dicts name (bytes), data (bytes), method 0 / 8, comp (bytes, optional: the
    compressed data), flags, made_by, external, usize / csize / crc overrides."""
    out, central = b'', b''
    for e in entries:
        name = e['name']
        data = e.get('data', b'')
        method = e.get('method', 8)
        comp = e.get('comp', deflate(data) if method == 8 else data)
        crc = e.get('crc', zlib.crc32(data) & 0xFFFFFFFF)
        usize = e.get('usize', len(data))
        csize = e.get('csize', len(comp))
        flags = e.get('flags', 0)
        offset = len(out)
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, method, 0, 0x21, crc, csize, usize, len(name), 0) + name + comp
        central += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, e.get('made_by', 0x0014), 20, flags, method, 0, 0x21,
                               crc, csize, usize, len(name), 0, 0, 0, 0, e.get('external', 0), offset) + name
    count = len(entries) if eocd_entries is None else eocd_entries
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, count, count, len(central), len(out), 0)
    return out + central + eocd

def write(name, zip_bytes, expected):
    open(os.path.join(HERE, name + '.zip'), 'wb').write(zip_bytes)
    open(os.path.join(HERE, name + '.expected'), 'w', newline='\n', encoding='utf-8').write(expected + '\n')

def ok_lines(entries):
    lines = ['ok']
    for e in entries:
        if e['name'].endswith(b'/'):
            continue
        d = e.get('data', b'')
        lines.append('%s %d %08x' % (e['name'].decode('utf-8'), len(d), zlib.crc32(d) & 0xFFFFFFFF))
    return '\n'.join(lines)

rnd = random.Random(29)
text = (b'export function draw(canvas, dock) {\n  canvas.text("hello", 4, 4);\n}\n' * 300)
noise = bytes(rnd.getrandbits(8) for _ in range(40000))
mixed = b''.join(rnd.choice([b'abc', b'abcabcabc', b'\x00' * 50, noise[:200], b'widget ']) for _ in range(300))

# 1. A good widget in one top-level folder, every way of compressing.
good = [
    {'name': b'my-widget/', 'method': 0},
    {'name': b'my-widget/widget.json', 'data': b'{ "name": "Mine", "version": "1.0.0" }\n'},
    {'name': b'my-widget/main.js', 'data': text},
    {'name': b'my-widget/lib/stored.js', 'data': b'export const x = 1;\n', 'method': 0},
    {'name': b'my-widget/assets/noise.bin', 'data': noise},
    {'name': b'my-widget/empty.txt', 'data': b''},
]
write('good-folder', archive(good), ok_lines(good))

inflate = []
for level in range(0, 10):
    for strategy, sname in ((zlib.Z_DEFAULT_STRATEGY, 'default'), (zlib.Z_FIXED, 'fixed'), (zlib.Z_HUFFMAN_ONLY, 'huffman'),
                            (zlib.Z_RLE, 'rle'), (zlib.Z_FILTERED, 'filtered')):
        for dname, data in (('text', text), ('noise', noise[:5000]), ('mixed', mixed)):
            inflate.append({'name': ('l%d-%s-%s.bin' % (level, sname, dname)).encode(), 'data': data,
                            'comp': deflate(data, level, strategy)})
inflate.append({'name': b'widget.json', 'data': b'{}'})
inflate.append({'name': b'one-byte.bin', 'data': b'x'})
inflate.append({'name': b'zeros.bin', 'data': b'\x00' * 60000})  # ratio ~600, but below 64 KB: allowed
write('inflate-corpus', archive(inflate), ok_lines(inflate))

# 2. Names.
def bad_name(case, name, why, utf8=False):
    write(case, archive([{'name': name, 'data': b'x', 'flags': 0x800 if utf8 else 0}]), 'error ' + why)
bad_name('name-dotdot', b'w/../../evil.js', '".." segment')
bad_name('name-absolute', b'/etc/evil.js', 'absolute path')
bad_name('name-backslash-absolute', b'\\\\server\\share\\x.js', 'absolute path')
bad_name('name-drive', b'C:/Windows/evil.js', "':'")
bad_name('name-stream', b'w/main.js:hidden', "':'")
bad_name('name-device', b'w/con.txt', 'reserved device name')
bad_name('name-device-com', b'w/COM1', 'reserved device name')
bad_name('name-trailing-dot', b'w/main.js.', 'ending in a dot')
bad_name('name-control', b'w/a\x01b.js', 'control character')
bad_name('name-nonascii', b'w/caf\xe9.js', 'non-ASCII')
bad_name('name-badutf8', b'w/caf\xc3.js', 'not valid UTF-8', utf8=True)
bad_name('name-empty-segment', b'w//main.js', 'empty folder name')
utf8 = [{'name': 'w/café.js'.encode('utf-8'), 'data': b'1', 'flags': 0x800}]
write('name-utf8-ok', archive(utf8), ok_lines(utf8))
write('name-duplicate', archive([{'name': b'w/Main.js', 'data': b'1'}, {'name': b'w/main.JS', 'data': b'2'}]), 'error in the archive twice')

# 3. Links and odd entries.
write('symlink-unix', archive([{'name': b'w/link', 'data': b'../../target', 'method': 0, 'made_by': 0x0314,
                                 'external': (0o120777 << 16)}]), 'error symbolic link')
write('device-unix', archive([{'name': b'w/fifo', 'data': b'', 'method': 0, 'made_by': 0x0314,
                                'external': (0o010644 << 16)}]), 'error not a plain file')
write('reparse-windows', archive([{'name': b'w/junction/', 'method': 0, 'made_by': 0x0A14, 'external': 0x410}]), 'error reparse point')
write('encrypted', archive([{'name': b'w/a.js', 'data': b'x', 'flags': 1}]), 'error encrypted')
write('method-bzip2', archive([{'name': b'w/a.js', 'data': b'x', 'method': 12, 'comp': b'x'}]), 'error compression method 12')

# 4. Limits.
many = [{'name': ('w/f%03d.js' % i).encode(), 'data': b'1'} for i in range(201)]
write('too-many-entries', archive(many), 'error 201 entries')
bomb = b'\x00' * (6 << 20)
write('ratio-bomb', archive([{'name': b'w/bomb.bin', 'data': bomb}]), 'error compressed more than 100 to 1')
write('file-too-large', archive([{'name': b'w/big.bin', 'data': noise, 'usize': 9 << 20, 'crc': 0}]), 'error larger than 8 MB')
# Each declares 680 KB from 7 KB of data (99 to 1: just under the ratio), so the total trips first.
part = noise[:7000]
total = [{'name': ('w/part%d.bin' % i).encode(), 'data': part, 'usize': 680 * 1024} for i in range(31)]
write('total-too-large', archive(total), 'error add up to more than 20 MB')
# Declared smaller than it inflates to: the decoder stops at the declared size.
write('lies-about-size', archive([{'name': b'w/a.bin', 'data': text, 'usize': 100}]), 'error larger than it says')
write('bad-crc', archive([{'name': b'w/a.js', 'data': text, 'crc': 0x12345678}]), 'error CRC-32')
write('stored-size-mismatch', archive([{'name': b'w/a.js', 'data': b'abcdef', 'method': 0, 'csize': 3}]), 'error does not match')
write('truncated-deflate', archive([{'name': b'w/a.js', 'data': text, 'comp': deflate(text)[:40]}]), 'error deflate')
write('garbage-deflate', archive([{'name': b'w/a.js', 'data': text, 'comp': bytes([0xFF] * 64)}]), 'error deflate')
write('not-a-zip', b'this is not a zip archive at all, just text' * 3, 'error not a zip archive')
damaged = bytearray(archive(good))
cd = damaged.rfind(struct.pack('<I', 0x02014b50))
damaged[cd:cd + 4] = b'XXXX'
write('damaged-directory', bytes(damaged), 'error central directory is damaged')
