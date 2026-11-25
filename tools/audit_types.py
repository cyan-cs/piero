"""Read-only inventory of integer-type smells in first-party C. Changes nothing.

  python tools/audit_types.py <output.json>

Heuristic, line based, comments and strings blanked: it counts candidates for a
human to read. A count is not a defect. What it looks for:

  narrow_cast   a (DWORD)/(int)/(unsigned)/(uint32_t)/(uint16_t)/(uint8_t) cast whose operand
                mentions a size-like word (len, size, count, bytes, n, offset, ...)
  int_size      a declaration "int <size-like name>" (sizes and counts want size_t / uint64_t)
  plain_long    "long" or "unsigned long" where the Win32 type DWORD / LONG is not meant
  unchecked_mul malloc/calloc/realloc whose size argument multiplies without an
                overflow check anywhere in the function
"""
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_resources import blank, functions  # noqa: E402  (same blanking and function finder)

ROOT = Path(__file__).resolve().parent.parent
SIZE_WORD = r'(?:len|length|size|count|bytes|byte|offset|off|cap|capacity|total|nbytes|used|avail|room|units|chars)'
NARROW = re.compile(r'\(\s*(DWORD|int|unsigned|unsigned int|uint32_t|uint16_t|uint8_t|UINT|WORD|BYTE|short|char)\s*\)\s*'
                    r'(?:\(?[^;,\)]*\b' + SIZE_WORD + r'\w*)', re.I)
INT_SIZE = re.compile(r'^\s*(?:static\s+)?(?:const\s+)?int\s+(\w*' + SIZE_WORD + r'\w*)\s*[=;,\)]', re.I | re.M)
PLAIN_LONG = re.compile(r'\b(?:unsigned\s+)?long\b(?!\s+long)')
ALLOC_MUL = re.compile(r'\b(malloc|realloc)\s*\(([^;]*?\*[^;]*?)\)|\bcalloc\s*\(')
OVERFLOW_GUARD = re.compile(r'SIZE_MAX|UINT64_MAX|UINT32_MAX|MAXDWORD|INT_MAX|_mul_overflow|wf_mul|checked_|overflow', re.I)


def first_party_files():
    out = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', 'src', 'tools', 'tests'],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout.split('\n')
    return [f for f in out if f.endswith(('.c', '.h')) and 'third_party' not in f]


def area(rel):
    return rel.split('/')[0] if not rel.startswith('src/') else 'src/' + rel.split('/')[1]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    by_area, details = {}, {'narrow_cast': [], 'int_size': [], 'plain_long': [], 'unchecked_mul': []}
    files = 0
    for rel in first_party_files():
        p = ROOT / rel
        if not p.is_file():
            continue
        files += 1
        text = re.sub(r'long\s+long', 'LL', blank(p.read_text(encoding='utf-8', errors='replace')))
        a = by_area.setdefault(area(rel), dict(files=0, lines=0, narrow_cast=0, int_size=0, plain_long=0,
                                               unchecked_mul=0))
        a['files'] += 1
        a['lines'] += text.count('\n') + 1
        for kind, rx in (('narrow_cast', NARROW), ('int_size', INT_SIZE), ('plain_long', PLAIN_LONG)):
            for m in rx.finditer(text):
                a[kind] += 1
                details[kind].append((rel, text.count('\n', 0, m.start()) + 1, m.group(0).strip()[:90]))
        if rel.endswith('.c'):
            for name, line, body in functions(text):
                for m in ALLOC_MUL.finditer(body):
                    arg = m.group(2) or ''
                    if m.group(0).startswith('calloc'):
                        continue
                    if '*' in arg and 'sizeof' not in arg.replace('sizeof *', '').replace('sizeof(', '') \
                            or re.search(r'\w\s*\*\s*\w', arg) and not OVERFLOW_GUARD.search(body):
                        if not OVERFLOW_GUARD.search(body):
                            a['unchecked_mul'] += 1
                            details['unchecked_mul'].append((rel, line, f'{name}: {m.group(0)[:70]}'))
    report = dict(files_scanned=files, by_area=by_area,
                  totals={k: sum(v[k] for v in by_area.values()) for k in
                          ('lines', 'narrow_cast', 'int_size', 'plain_long', 'unchecked_mul')},
                  details={k: v[:400] for k, v in details.items()})
    Path(sys.argv[1]).write_text(json.dumps(report, indent=1), encoding='utf-8')
    print('files scanned:', files, ' totals:', report['totals'])
    for name, v in sorted(by_area.items()):
        print(f"  {name:16s} files={v['files']:3d} lines={v['lines']:6d} narrow_cast={v['narrow_cast']:4d} "
              f"int_size={v['int_size']:3d} plain_long={v['plain_long']:3d} unchecked_mul={v['unchecked_mul']:2d}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
