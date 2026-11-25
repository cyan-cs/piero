"""Makes the archives tools/bench_archive.exe measures, in a folder you name.

    python tools/make_archive_bench.py <folder>

Nothing here is committed (they are tens of megabytes): a tree of 20000 small
text files in 200 folders as .zip, .7z, .tar.gz, .tar.xz and .tar.zst, and one
256 MiB text file as .tar.gz and .tar.zst (the solid case: to get past it the
whole stream has to be decompressed).
"""
import os, random, shutil, subprocess, sys, zipfile

TAR = r'C:\Windows\System32\tar.exe'


def words(rng, n):
    vocab = ['alpha', 'beta', 'gamma', 'delta', 'index', 'archive', 'piero',
             'folder', 'preview', 'cursor', 'column', 'listing', '0123', '4567']
    return ' '.join(rng.choice(vocab) for _ in range(n))


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    rng = random.Random(7)
    tree = os.path.join(out, '_tree')
    shutil.rmtree(tree, ignore_errors=True)
    for d in range(200):
        dd = os.path.join(tree, 'dir%03d' % d)
        os.makedirs(dd)
        for f in range(100):
            with open(os.path.join(dd, 'file%03d.txt' % f), 'w') as fh:
                fh.write(words(rng, 300))
    with zipfile.ZipFile(os.path.join(out, 'many.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for d in sorted(os.listdir(tree)):
            for f in sorted(os.listdir(os.path.join(tree, d))):
                z.write(os.path.join(tree, d, f), d + '/' + f)
    for name, args in [('many.7z', ['--format', '7zip']), ('many.tar.gz', ['-z']),
                       ('many.tar.xz', ['-J']), ('many.tar.zst', ['--zstd'])]:
        p = os.path.join(out, name)
        if os.path.exists(p):
            os.remove(p)
        subprocess.check_call([TAR, *args, '-cf', p, '-C', tree, '.'])
    shutil.rmtree(tree)
    big = os.path.join(out, '_big')
    shutil.rmtree(big, ignore_errors=True)
    os.makedirs(big)
    with open(os.path.join(big, 'big.txt'), 'w') as fh:
        chunk = words(rng, 20000) + '\n'
        for i in range(256 * 1024 * 1024 // len(chunk) + 1):
            fh.write(chunk if i % 3 else words(rng, 20000) + '\n')
    for name, args in [('big.tar.gz', ['-z']), ('big.tar.zst', ['--zstd'])]:
        p = os.path.join(out, name)
        if os.path.exists(p):
            os.remove(p)
        subprocess.check_call([TAR, *args, '-cf', p, '-C', big, '.'])
    shutil.rmtree(big)
    for f in sorted(os.listdir(out)):
        print('%12d  %s' % (os.path.getsize(os.path.join(out, f)), f))


if __name__ == '__main__':
    main()
