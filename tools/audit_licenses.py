"""Inventory vendor C translation units and distribution notice files.

This is not a legal/compliance verdict. Individual upstream terms control.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('build', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
cache = (args.build / 'CMakeCache.txt').read_text(encoding='utf-8')
ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$', cache, re.M)[1]
commands = json.loads(subprocess.check_output([ninja, '-C', str(args.build), '-t', 'compdb']))
sources = set()
for command in commands:
    source = Path(command['file'])
    if not source.is_absolute():
        source = Path(command['directory']) / source
    source = source.resolve()
    if source.is_relative_to(root / 'src/third_party') and source.suffix == '.c':
        sources.add(source)
rows = []
for source in sorted(sources):
    data = source.read_bytes()
    head = data[:16000].lower()
    notice = any(s in head for s in (b'copyright', b'public domain', b'license', b'licence'))
    rows.append(dict(path=source.relative_to(root).as_posix(), sha256=hashlib.sha256(data).hexdigest(), notice_marker=notice))
report = dict(compiled_vendor_sources=rows, notices=[dict(path=p.relative_to(root).as_posix(), sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted((root/'LICENSES').glob('*.txt'))],
              project_license_present=(root/'LICENSE').is_file(), verdict='OWNER REVIEW REQUIRED')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(f'Vendor C translation units: {len(rows)}; distribution notices: {len(report["notices"])}')
print('Sources needing notice context:', [r['path'] for r in rows if not r['notice_marker']])
print('Project license:', 'present; review terms' if report['project_license_present'] else 'UNRESOLVED')
if not rows:
    raise SystemExit('No compiled vendor sources were found; inventory is incomplete')
