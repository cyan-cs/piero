"""Read-only Git publication audit. Never print matched credential values.

This is a conservative review aid, not proof that a repository has no secrets.
The JSON report contains locations/categories only; keep it local until reviewed.
Incomplete Git replies, worktree reparse paths and objects/files over 64 MiB
stop the audit without writing a complete report.
"""
import argparse
import json
import re
import stat
import subprocess
from pathlib import Path


def git(*args):
    result = subprocess.run(['git', *args], capture_output=True)
    if result.returncode:
        raise RuntimeError('Git inventory failed')
    return result.stdout


READ_LIMIT = 64 * 1024 * 1024


def historical_blobs(objects, inspect, large):
    scanned = 0
    with subprocess.Popen(['git', 'cat-file', '--batch'], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
        try:
            for oid, path in objects.items():
                proc.stdin.write((oid + '\n').encode('ascii'))
                proc.stdin.flush()
                line = proc.stdout.readline(256)
                header = line.split()
                if (not line.endswith(b'\n') or len(header) != 3 or
                        header[0] != oid.encode('ascii') or
                        header[1] not in (b'blob', b'tree', b'commit', b'tag') or
                        not header[2].isdigit()):
                    raise RuntimeError('Invalid historical object header')
                size = int(header[2])
                if size > READ_LIMIT:
                    raise RuntimeError('Historical object exceeds audit read limit')
                data = proc.stdout.read(size)
                if len(data) != size or proc.stdout.read(1) != b'\n':
                    raise RuntimeError('Truncated historical object')
                if header[1] != b'blob':
                    continue
                location = dict(object=oid, path=path)
                if size > 1024 * 1024:
                    large.append(dict(**location, bytes=size))
                inspect(data, location)
                scanned += 1
            proc.stdin.close()
            if proc.stdout.read(1):
                raise RuntimeError('Unexpected historical object output')
            if proc.wait():
                raise RuntimeError('Historical object process failed')
        except BaseException:
            # The child may still be writing the rejected object to its pipe.
            if proc.poll() is None:
                proc.kill()
            raise
    return scanned


def worktree_bytes(root, name):
    relative = Path(name)
    if not relative.parts or relative.is_absolute() or '..' in relative.parts:
        raise RuntimeError('Unsafe worktree inventory path')
    path = root
    for part in relative.parts:
        path /= part
        try:
            info = path.lstat()
        except FileNotFoundError:
            return None
        if (stat.S_ISLNK(info.st_mode) or
                getattr(info, 'st_file_attributes', 0) & 0x400):
            raise RuntimeError('Worktree reparse path requires manual review')
    if not stat.S_ISREG(info.st_mode):
        return None
    with path.open('rb') as stream:
        data = stream.read(READ_LIMIT + 1)
    if len(data) > READ_LIMIT:
        raise RuntimeError('Worktree file exceeds audit read limit')
    return data


RULES = {
    'private-key': rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----',
    'service-token': rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|sk-[A-Za-z0-9_-]{32,})',
    'credential-assignment': rb'(?i)(?:password|passwd|api[_-]?key|access[_-]?token|client[_-]?secret|authorization)["\x27]?\s*[:=]\s*["\x27][^"\x27\r\n]{8,}["\x27]',
    'user-profile-path': rb'(?i)(?:[A-Z]:[\\/](?:Users|Documents and Settings)[\\/][^\s"\x27<>]+|/home/[^\s/]+)',
    'email': rb'[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}',
}


def audit():
    rows = git('rev-list', '--objects', '--all').splitlines()
    objects = {row.split(b' ', 1)[0].decode(): row.split(b' ', 1)[1].decode('utf-8', 'replace')
               for row in rows if b' ' in row}
    findings, large, binary = [], [], []
    def inspect(data, location):
        if b'\0' in data[:8192]:
            binary.append(location)
        for category, pattern in RULES.items():
            lines = sorted({data[:m.start()].count(b'\n') + 1 for m in re.finditer(pattern, data)})
            if lines:
                findings.append(dict(location=location, category=category, lines=lines))
    scanned = historical_blobs(objects, inspect, large)
    root = Path(git('rev-parse', '--show-toplevel').decode('utf-8').strip())
    for path in git('-C', str(root), 'ls-files', '--full-name', '-z', '--cached', '--others', '--exclude-standard').decode('utf-8').split('\0'):
        if path:
            data = worktree_bytes(root, path)
            if data is not None:
                inspect(data, dict(worktree=path))
    commits = git('log', '--all', '--format=%H%x00%an%x00%ae%x00%cn%x00%ce%x00%B%x00').decode('utf-8', 'replace')
    metadata = dict(email_matches=len(re.findall(RULES['email'].decode(), commits)),
                    note='Author/committer names and email addresses are public metadata; owner review required.')
    for category in ('private-key', 'service-token', 'credential-assignment', 'user-profile-path'):
        if re.search(RULES[category], commits.encode()):
            findings.append(dict(location='commit-metadata', category=category))
    return dict(audit_status='COMPLETE', head=git('rev-parse', 'HEAD').decode().strip(),
                  refs=git('for-each-ref', '--format=%(refname)').decode().splitlines(),
                  commits=int(git('rev-list', '--all', '--count')), historical_blobs=scanned,
                  findings=findings, large_objects=large, binary_locations=binary, metadata=metadata)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', required=True)
    args = ap.parse_args()
    try:
        report = audit()
    except (OSError, ValueError, RuntimeError):
        print('Publication audit: INCOMPLETE; inventory could not be read safely')
        return 1
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('head', 'commits', 'historical_blobs', 'metadata')}))
    print('Finding categories:', {k: sum(f['category'] == k for f in report['findings']) for k in RULES})
    print('Large objects:', len(report['large_objects']), 'Binary locations:', len(report['binary_locations']))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
