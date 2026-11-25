"""High-confidence local/CI guard. Reports locations, never matched values."""
import argparse
import os
import re
import subprocess
from pathlib import Path

PATTERNS = [rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----',
            rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|sk-[A-Za-z0-9_-]{32,})']


def git(*args):
    result = subprocess.run(['git', *args], capture_output=True)
    if result.returncode:
        raise RuntimeError('Git inventory failed')
    return result.stdout


def local_only(path):
    parts = Path(path.lower()).parts
    directories = {'.superpowers', '.codex', '.agents', '.claude', '.aws', '.ssh',
                   '.vs', '.vscode', '.idea', 'cmakefiles'}
    name = parts[-1]
    return (bool(set(parts[:-1]) & directories) or
            parts[0] in {'out', 'build'} or parts[0].startswith(('build-', 'cmake-build-')) or
            name in {'.mcp.json', 'cmakeuserpresets.json', 'cmakecache.txt'} or
            (name.startswith('.env') and name != '.env.example' and (name == '.env' or name.startswith('.env.'))) or
            Path(name).suffix in {'.pfx', '.p12', '.pem', '.key', '.exe', '.dll', '.obj', '.pdb', '.dmp', '.stackdump'})


def has_secret(data):
    return any(re.search(pattern, data) for pattern in PATTERNS)


def scan(history):
    if history and git('rev-parse', '--is-shallow-repository').strip() != b'false':
        raise RuntimeError('History scan requires a complete checkout (fetch-depth: 0)')
    tracked = {os.fsdecode(p) for p in git('ls-files', '-z', '--cached').split(b'\0') if p}
    paths = {os.fsdecode(p) for p in git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0') if p}
    bad = []
    for name in sorted(paths):
        # Check tracked names even when the local file is missing. An ignore
        # rule does not remove an already committed local setting or binary.
        if name in tracked and local_only(name):
            bad.append((repr(name), 'tracked local configuration/credential/generated file'))
        path = Path(name)
        if path.is_symlink():
            bad.append((repr(name), 'symbolic link requires source review'))
        elif path.is_file() and has_secret(path.read_bytes()):
            bad.append((repr(name), 'credential/key pattern'))
    if history:
        rows = git('rev-list', '--objects', '--all').splitlines()
        proc = subprocess.Popen(['git', 'cat-file', '--batch'], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            for row in rows:
                oid = row.split(b' ', 1)[0]
                proc.stdin.write(oid + b'\n')
                proc.stdin.flush()
                head = proc.stdout.readline().split()
                if len(head) != 3:
                    raise RuntimeError('Incomplete historical object inventory')
                size = int(head[2])
                data = proc.stdout.read(size)
                if len(data) != size or proc.stdout.read(1) != b'\n':
                    raise RuntimeError('Truncated historical object')
                if has_secret(data):
                    bad.append((oid.decode('ascii'), 'historical credential/key pattern'))
        finally:
            proc.stdin.close()
            proc.stdout.close()
            code = proc.wait()
        if code:
            raise RuntimeError('Historical object scan failed')
    for location, category in bad:
        print(f'{location}: {category}')
    print('Secret/config guard:', 'FAIL' if bad else 'PASS', '(manual source review is also required)')
    return bool(bad)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history', action='store_true')
    args = parser.parse_args()
    try:
        raise SystemExit(scan(args.history))
    except (OSError, ValueError, RuntimeError) as error:
        # Only our fixed diagnostic strings are safe; OS/value errors may
        # contain path or source bytes and are deliberately not echoed.
        message = str(error) if isinstance(error, RuntimeError) else 'Inventory could not be read completely'
        print('Secret/config guard: FAIL; ' + message)
        raise SystemExit(1)
