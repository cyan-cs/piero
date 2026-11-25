"""Regenerates tests/archive/*: small archives of the formats piero reads.

Needs Windows' own tar.exe (bsdtar, built on libarchive) for 7z / zstd / xz and
Python for the ZIP64 and empty ones. The files are committed; run this only to
change them. Every archive holds the same tree unless noted:

    README.md
    src/main.c
    src/utils.c
    docs/                       (an empty folder)
    docs/deep/a/b/c/d.txt
    日本語/ファイル.txt            (UTF-8 names)
"""
import gzip, lzma, os, shutil, struct, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'tests', 'archive')
TAR = r'C:\Windows\System32\tar.exe'

TREE = {
    'README.md': b'# piero fixture\n\nhello from the archive\n',
    'src/main.c': b'int main(void)\n{\n\treturn 0;\n}\n',
    'src/utils.c': b'/* utils */\nint util(void) { return 1; }\n',
    'docs/deep/a/b/c/d.txt': b'deep file\n',
    '\u65e5\u672c\u8a9e/\u30d5\u30a1\u30a4\u30eb.txt': '\u65e5\u672c\u8a9e\u306e\u30c6\u30ad\u30b9\u30c8\n'.encode('utf-8'),
}
EMPTY_DIRS = ['docs']
SINGLE = b'single compressed file\nline two\n'


def stage():
    root = os.path.join(OUT, '_stage')
    shutil.rmtree(root, ignore_errors=True)
    for rel, data in TREE.items():
        p = os.path.join(root, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
    for d in EMPTY_DIRS:
        os.makedirs(os.path.join(root, d), exist_ok=True)
    return root


def bsdtar(root, name, *args):
    path = os.path.join(OUT, name)
    if os.path.exists(path):
        os.remove(path)
    # "." rather than the names: tar.exe takes its arguments in the ANSI code
    # page, which cannot spell the Japanese ones
    subprocess.check_call([TAR, *args, '-cf', path, '-C', root, '.'])


def main():
    os.makedirs(OUT, exist_ok=True)
    root = stage()
    bsdtar(root, 'basic.zip', '--format', 'zip')
    bsdtar(root, 'basic.7z', '--format', '7zip')
    bsdtar(root, 'basic.tar')
    bsdtar(root, 'basic.tar.gz', '-z')
    bsdtar(root, 'basic.tar.xz', '-J')
    bsdtar(root, 'basic.tar.zst', '--zstd')
    shutil.copyfile(os.path.join(OUT, 'basic.tar.gz'), os.path.join(OUT, 'basic.tgz'))
    shutil.copyfile(os.path.join(OUT, 'basic.tar.xz'), os.path.join(OUT, 'basic.txz'))
    shutil.copyfile(os.path.join(OUT, 'basic.tar.zst'), os.path.join(OUT, 'basic.tzst'))
    # a 7z made of many small files (one solid block)
    solid = os.path.join(OUT, '_solid')
    shutil.rmtree(solid, ignore_errors=True)
    os.makedirs(solid)
    for i in range(40):
        with open(os.path.join(solid, 'f%02d.txt' % i), 'wb') as f:
            f.write(('file %d\n' % i).encode() * 50)
    bsdtar(solid, 'solid.7z', '--format', '7zip')
    shutil.rmtree(solid)

    # single compressed files
    with open(os.path.join(OUT, '_single.txt'), 'wb') as f:
        f.write(SINGLE)
    with gzip.GzipFile(os.path.join(OUT, 'single.txt.gz'), 'wb', mtime=0) as f:
        f.write(SINGLE)
    with lzma.open(os.path.join(OUT, 'single.txt.xz'), 'wb') as f:
        f.write(SINGLE)
    subprocess.check_call([TAR, '--zstd', '-cf', os.path.join(OUT, '_tmp.tar.zst'), '-C', OUT, '_single.txt'])
    os.remove(os.path.join(OUT, '_single.txt'))
    os.remove(os.path.join(OUT, '_tmp.tar.zst'))
    # a bare zstd frame: bsdtar's --zstd gives a tar stream, so write the frame
    # by hand: a single raw block holding SINGLE
    frame = b'\x28\xb5\x2f\xfd'                      # magic
    frame += b'\x20' + bytes([len(SINGLE)])          # single segment, 1-byte size
    n = len(SINGLE)
    frame += struct.pack('<I', (n << 3) | 1)[:3]  # raw block, last block
    frame += SINGLE
    with open(os.path.join(OUT, 'single.txt.zst'), 'wb') as f:
        f.write(frame)

    # ZIP64: one entry that carries the 64-bit extra field and zip64 end record
    with zipfile.ZipFile(os.path.join(OUT, 'zip64.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        zi = zipfile.ZipInfo('big/forced64.txt', (2024, 1, 2, 3, 4, 6))
        zi.compress_type = zipfile.ZIP_DEFLATED
        with z.open(zi, 'w', force_zip64=True) as f:
            f.write(b'zip64 content\n' * 10)
    # empty archives
    with zipfile.ZipFile(os.path.join(OUT, 'empty.zip'), 'w'):
        pass
    subprocess.check_call([TAR, '-cf', os.path.join(OUT, 'empty.tar'), '-T', os.devnull])
    # a tar.gz that expands to far more than it weighs: 4 MiB of zeros
    bomb = os.path.join(OUT, '_bomb')
    shutil.rmtree(bomb, ignore_errors=True)
    os.makedirs(bomb)
    with open(os.path.join(bomb, 'zeros.bin'), 'wb') as f:
        f.write(bytes(4 << 20))
    bsdtar(bomb, 'bomb.tar.gz', '-z')
    shutil.rmtree(bomb)
    # links: a symbolic and a hard link entry, a name that climbs out, a fifo
    import io, tarfile
    with tarfile.open(os.path.join(OUT, 'links.tar'), 'w') as t:
        def member(name, data=b'', type=tarfile.REGTYPE, link=''):
            ti = tarfile.TarInfo(name)
            ti.type = type
            ti.linkname = link
            ti.size = len(data) if type == tarfile.REGTYPE else 0
            t.addfile(ti, io.BytesIO(data) if type == tarfile.REGTYPE else None)
        member('real.txt', b'the real file\n')
        member('sym', type=tarfile.SYMTYPE, link='real.txt')
        member('hard', type=tarfile.LNKTYPE, link='real.txt')
        member('../escape.txt', b'must not appear\n')
        member('fifo', type=tarfile.FIFOTYPE)
    # an encrypted ZIP (traditional PKWARE encryption), password "secret"
    enc = os.path.join(OUT, '_enc')
    shutil.rmtree(enc, ignore_errors=True)
    os.makedirs(enc)
    with open(os.path.join(enc, 'secret.txt'), 'wb') as f:
        f.write(b'top secret\n')
    subprocess.check_call([TAR, '--format', 'zip', '--options', 'zip:encryption=zipcrypt',
                           '--passphrase', 'secret', '-cf', os.path.join(OUT, 'encrypted.zip'),
                           '-C', enc, '.'])
    shutil.rmtree(enc)
    shutil.rmtree(root)
    for f in sorted(os.listdir(OUT)):
        print('%8d  %s' % (os.path.getsize(os.path.join(OUT, f)), f))


if __name__ == '__main__':
    main()
