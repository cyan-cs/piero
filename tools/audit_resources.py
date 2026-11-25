"""Read-only audit of first-party C for resource-handling smells. Changes nothing.

  python tools/audit_resources.py <output.json>

Heuristic, not a parser: functions are found by their column-0 signature and
brace matching, and comments and string literals are blanked first. It reports
candidates for a human to read; a finding is not a defect until it is read.

  sentinel   a handle from an API that fails with INVALID_HANDLE_VALUE tested
             against NULL, or one that fails with NULL tested against
             INVALID_HANDLE_VALUE
  cleanup    a function that acquires two or more kinds of resource, has three
             or more returns and no goto: a candidate for one cleanup label
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# API -> which value means failure
FAILS_WITH_INVALID = ('CreateFileW', 'CreateFileA', 'CreateFile2', 'FindFirstFileW', 'FindFirstFileExW',
                      'FindFirstStreamW', 'CreateNamedPipeW')
FAILS_WITH_NULL = ('CreateEventW', 'CreateMutexW', 'CreateThread', 'OpenProcess', 'CreateFileMappingW',
                   'CreateJobObjectW', 'OpenProcessToken')
ACQUIRE = [
    ('heap', re.compile(r'\b(malloc|calloc|realloc|_wcsdup|_strdup)\s*\(')),
    ('file-handle', re.compile(r'\b(CreateFileW|FindFirstFileW|FindFirstFileExW|FindFirstStreamW|CreateNamedPipeW)\s*\(')),
    ('kernel-handle', re.compile(r'\b(CreateEventW|CreateMutexW|CreateThread|OpenProcess|CreateJobObjectW|DuplicateHandle|OpenProcessToken)\s*\(')),
    ('lock', re.compile(r'\b(AcquireSRWLock(Exclusive|Shared)|EnterCriticalSection)\s*\(')),
    ('com', re.compile(r'\b(CoCreateInstance|CoInitializeEx)\s*\(')),
]


def blank(src: str) -> str:
    """Comments and string/char literals replaced by spaces, line structure kept."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


FUNC = re.compile(r'^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)


def functions(text: str):
    for m in FUNC.finditer(text):
        name = m.group(1)
        if name in ('if', 'for', 'while', 'switch', 'sizeof'):
            continue
        start = m.end() - 1
        depth, j = 0, start
        while j < len(text):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield name, text.count('\n', 0, m.start()) + 1, text[start:j + 1]


def check_sentinels(body: str):
    found = []
    # the whole lvalue ("o->directory", not just "directory"): a local of the same
    # name as a member is a different object
    for var_m in re.finditer(r'((?:\b[A-Za-z_]\w*(?:->|\.))*[A-Za-z_]\w*)\s*=\s*(\w+)\s*\(', body):
        var, api = var_m.group(1), var_m.group(2)
        if api in FAILS_WITH_INVALID:
            bad = re.search(r'(?:\bif\s*\(\s*!\s*%s\s*\)|\b%s\s*(?:==|!=)\s*NULL\b|NULL\s*(?:==|!=)\s*%s\b)'
                            % ((re.escape(var),) * 3), body)
            if bad:
                found.append((var, api, 'compared with NULL; this API fails with INVALID_HANDLE_VALUE'))
        elif api in FAILS_WITH_NULL:
            bad = re.search(r'\b%s\s*(?:==|!=)\s*INVALID_HANDLE_VALUE\b' % re.escape(var), body)
            if bad:
                found.append((var, api, 'compared with INVALID_HANDLE_VALUE; this API fails with NULL'))
    return found


def first_party_files():
    listed = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', 'src', 'tools', 'tests'],
                            cwd=ROOT, capture_output=True, text=True, check=True).stdout.split('\n')
    return [f for f in listed if f.endswith('.c') and 'third_party' not in f]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    sentinel, cleanup, scanned = [], [], 0
    for rel in first_party_files():
        p = ROOT / rel
        if not p.is_file():
            continue
        text = blank(p.read_text(encoding='utf-8', errors='replace'))
        scanned += 1
        for name, line, body in functions(text):
            for var, api, why in check_sentinels(body):
                sentinel.append(dict(file=rel, line=line, function=name, variable=var, api=api, why=why))
            kinds = {k for k, rx in ACQUIRE if rx.search(body)}
            returns = len(re.findall(r'\breturn\b', body))
            if len(kinds) >= 2 and returns >= 3 and not re.search(r'\bgoto\b', body):
                cleanup.append(dict(file=rel, line=line, function=name, kinds=sorted(kinds), returns=returns,
                                    lines=body.count('\n') + 1))
    cleanup.sort(key=lambda d: (-d['returns'] * len(d['kinds']), d['file']))
    report = dict(files_scanned=scanned, sentinel_findings=sentinel, cleanup_candidates=cleanup)
    Path(sys.argv[1]).write_text(json.dumps(report, indent=1), encoding='utf-8')
    print('files scanned:', scanned)
    print('handle sentinel mix-ups to read:', len(sentinel))
    print('cleanup-consolidation candidates:', len(cleanup))
    return 0


if __name__ == '__main__':
    sys.exit(main())
