"""Read-only candidate inventory. This never approves a product release."""
import argparse
import hashlib
import json
import stat
from language_catalog import load as load_language, load_bundled, FILE_LIMIT
from pathlib import Path

def members(root, required, failures):
    directories = {Path(name).parent.as_posix() for name in required} - {'.'}
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            name = path.relative_to(root).as_posix()
            info = path.lstat()
            # Junctions are reparse points too, but Path.is_symlink() is false.
            if (stat.S_ISLNK(info.st_mode) or
                    getattr(info, 'st_file_attributes', 0) & 0x400):
                failures.append('Unexpected link ' + name)
                continue
            if stat.S_ISDIR(info.st_mode) and name in directories:
                pending.append(path)
            elif name not in required:
                failures.append('Unreviewed package member ' + name)
            elif not stat.S_ISREG(info.st_mode):
                failures.append('Not a regular file ' + name)
            else:
                yield name, path


def digest(path, limit=None):
    total, tail, sanitizer = 0, b'', False
    hasher = hashlib.sha256()
    markers = (b'clang_rt.asan', b'clang_rt.ubsan')
    with path.open('rb') as stream:
        while data := stream.read(65536 if limit is None else min(65536, limit + 1 - total)):
            total += len(data)
            if limit is not None and total > limit:
                raise ValueError('File exceeds inventory limit')
            hasher.update(data)
            scanned = tail + data
            sanitizer |= any(marker in scanned for marker in markers)
            tail = scanned[-(max(map(len, markers)) - 1):]
    return total, hasher.hexdigest(), sanitizer


def inspect(root, repo, executable='piero.exe', installer_helper=False):
    required = [executable, 'LICENSE'] + [
        'LICENSES/' + p.name for p in (repo / 'LICENSES').glob('*.txt')]
    required += sorted({'languages/en.ini', 'languages/ja.ini'} | {
        'languages/' + p.name for p in (repo / 'languages').glob('*.ini')})
    if installer_helper:
        required.append('piero-path.exe')
    failures, files = [], []
    if not (repo / 'LICENSE').is_file():
        failures.append('Project LICENSE is missing: no release package can be approved')
    regular = dict(members(root, required, failures))
    for name in required:
        if name not in regular:
            failures.append('Missing ' + name)
    for name, path in regular.items():
        try:
            size, sha256, sanitizer = digest(
                path, FILE_LIMIT if name.startswith('languages/') else None)
        except ValueError:
            failures.append('Oversized language ' + name)
            continue
        if sanitizer:
            failures.append('Development sanitizer dependency ' + name)
        if name == 'LICENSE':
            if not (repo / name).is_file() or sha256 != digest(repo / name)[1]:
                failures.append('Project license differs from source LICENSE')
        elif name.startswith('LICENSES/') and sha256 != digest(repo / name)[1]:
            failures.append('License notice differs ' + name)
        files.append(dict(path=name, bytes=size, sha256=sha256))
    language_names = [name for name in required if name.startswith('languages/')]
    if all(name in regular for name in language_names):
        try:
            base_language = load_bundled(root / 'languages')[0]
            for name in language_names:
                packaged = load_language(root / name, base_language['arguments'], base_language['messages'])
                reviewed = load_language(repo / name, base_language['arguments'], base_language['messages'])
                if packaged['source_sha256'] != reviewed['source_sha256']:
                    failures.append('Language differs from reviewed source ' + name)
        except (OSError, ValueError, UnicodeError):
            failures.append('Missing or invalid language INI')
    return dict(files=files, total_bytes=sum(f['bytes'] for f in files),
                failures=failures, inventory_status='FAIL' if failures else 'PASS',
                release_status='RELEASE BLOCKED')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--executable', choices=['wfile.exe', 'piero.exe'], default='piero.exe')
    parser.add_argument('--installer-helper', action='store_true')
    args = parser.parse_args()
    report = inspect(args.directory.resolve(strict=True), Path(__file__).resolve().parent.parent,
                     args.executable, args.installer_helper)
    print(json.dumps(report, indent=2))
    raise SystemExit(bool(report['failures']))
