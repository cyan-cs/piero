"""Check compiled vendor sources/headers against distribution notices.

This checks notice coverage, not ownership, source provenance, or a legal verdict.
Individual upstream terms remain controlling. Do not regenerate notices blindly
when adding a dependency or changing its licensing terms.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = Path('src/third_party')
BUNDLES = {
    'libarchive-3.8.10': 'libarchive-3.8.10.txt',
    'zlib-1.3.1': 'zlib-1.3.1.txt',
    'xz-5.8.1': 'xz-liblzma-5.8.1.txt',
    'zstd-1.5.7': 'zstd-1.5.7.txt',
    'tree-sitter-0.27.2': 'tree-sitter-0.27.2.txt',
    'ts-grammars': 'tree-sitter-grammars.txt',
}
GRAMMARS = ('bash', 'ini', 'json', 'jsonc', 'markdown', 'markdown_inline',
            'powershell', 'toml', 'xml', 'yaml')
BASE_LICENSES = {
    'libarchive-3.8.10.txt': ['libarchive-3.8.10/COPYING'],
    'zlib-1.3.1.txt': ['zlib-1.3.1/LICENSE'],
    'xz-liblzma-5.8.1.txt': ['xz-5.8.1/COPYING.0BSD'],
    'zstd-1.5.7.txt': ['zstd-1.5.7/LICENSE'],
    'tree-sitter-0.27.2.txt': ['tree-sitter-0.27.2/LICENSE',
                             'tree-sitter-0.27.2/src/unicode/LICENSE'],
    'tree-sitter-grammars.txt': [f'ts-grammars/{name}/LICENSE' for name in GRAMMARS],
}
# The project's Windows configuration header is not an upstream component.
PROJECT_HEADERS = {VENDOR / 'libarchive-config/config.h'}
# Shared XML scanner code is covered by the XML grammar's MIT notice.
SHARED_GRAMMAR_HEADERS = {VENDOR / 'ts-grammars/common/scanner.h'}
MARKER = re.compile(r'copyright|public domain|\blicen[cs]e\b|SPDX-License-Identifier|'
                    r'redistribution and use|permission is hereby granted', re.I)


def normalized(text):
    return ' '.join(text.split())


def notice_blocks(text):
    comments = r'/\*.*?\*/|^[ \t]*//[^\n]*(?:\n[ \t]*//[^\n]*)*'
    return [m[0] for m in re.finditer(comments, text, re.S | re.M) if MARKER.search(m[0])]


def inventory(build, root=ROOT):
    build, root = build.resolve(), root.resolve()
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$', cache, re.M)[1]
    commands = json.loads(subprocess.check_output([ninja, '-C', str(build), '-t', 'compdb']))
    sources = set()
    for command in commands:
        source = Path(command['file'])
        if not source.is_absolute():
            source = Path(command['directory']) / source
        source = source.resolve()
        if source.is_relative_to(root / VENDOR) and source.suffix == '.c':
            sources.add(source)
    if not sources:
        raise ValueError('No compiled vendor sources; inventory is incomplete')
    deps = subprocess.check_output([ninja, '-C', str(build), '-t', 'deps'],
                                   encoding='utf-8', errors='strict')
    # Deduplicate before filesystem operations: headers occur in hundreds of TUs.
    names = {line.strip() for line in deps.splitlines()
             if line.startswith('    ') and 'third_party' in line}
    dependencies = set(sources)
    for name in sorted(names):
        path = Path(name)
        if not path.is_absolute():
            path = build / path
        path = path.resolve()
        if path.is_file() and path.is_relative_to(root / VENDOR):
            dependencies.add(path)
    if not any(p.suffix == '.h' for p in dependencies):
        raise ValueError('No compiled vendor headers; build first to collect dependencies')
    return sources, dependencies


def check_notices(paths, root=ROOT):
    root = root.resolve()
    texts = {name: normalized((root / 'LICENSES' / name).read_text(encoding='utf-8'))
             for name in BUNDLES.values()}
    failures = []
    for bundle, licenses in BASE_LICENSES.items():
        for name in licenses:
            if normalized((root / VENDOR / name).read_text(encoding='utf-8')) not in texts[bundle]:
                failures.append(f'Missing upstream license: {name} in LICENSES/{bundle}')
    for path in sorted(paths):
        relative = path.resolve().relative_to(root)
        if relative in PROJECT_HEADERS:
            continue
        vendor_path = relative.relative_to(VENDOR)
        component = vendor_path.parts[0]
        bundle = BUNDLES.get(component)
        if bundle is None:
            failures.append(f'Unreviewed vendor component: {relative.as_posix()}')
            continue
        if (component == 'ts-grammars' and vendor_path.parts[1] not in GRAMMARS
                and relative not in SHARED_GRAMMAR_HEADERS):
            failures.append(f'Unreviewed grammar: {relative.as_posix()}')
            continue
        for index, block in enumerate(notice_blocks(path.read_text(encoding='utf-8'))):
            if normalized(block) not in texts[bundle]:
                failures.append(f'Missing source notice {index + 1}: '
                                f'{relative.as_posix()} in LICENSES/{bundle}')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check-notices', action='store_true')
    args = parser.parse_args()
    sources, dependencies = inventory(args.build)
    def row(path):
        data = path.read_bytes()
        return dict(path=path.relative_to(ROOT).as_posix(),
                    sha256=hashlib.sha256(data).hexdigest(),
                    notice_marker=bool(MARKER.search(data[:16000].decode('utf-8', errors='replace'))))
    failures = check_notices(dependencies) + check_additional_notices()
    report = dict(compiled_vendor_sources=[row(p) for p in sorted(sources)],
                  compiled_vendor_dependencies=[row(p) for p in sorted(dependencies)],
                  notices=[dict(path=p.relative_to(ROOT).as_posix(),
                                sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                           for p in sorted((ROOT / 'LICENSES').glob('*.txt'))],
                  project_license_present=(ROOT / 'LICENSE').is_file(),
                  notice_failures=failures,
                  notice_coverage='FAIL' if failures else 'PASS',
                  verdict='Notice coverage only; ownership and provenance not certified')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Vendor C translation units: {len(sources)}; '
          f'compiled source/header dependencies: {len(dependencies)}; '
          f'distribution notices: {len(report["notices"])}')
    print('Notice coverage:', report['notice_coverage'])
    for failure in failures:
        print(failure)
    return int(args.check_notices and (bool(failures) or not report['project_license_present']))


def check_additional_notices(root=ROOT):
    """Cover reviewed data outside the vendor compiler inventory and source helpers."""
    required = {
        'LICENSES/unicode-data.txt': ('UNICODE LICENSE V3',
                                    'Copyright © 1991-2026 Unicode, Inc.',
                                    'https://www.unicode.org/Public/15.0.0/ucd/EastAsianWidth.txt',
                                    'modified representations'),
        'src/text/wf_width_ambiguous.h': ('LICENSES/unicode-data.txt',),
        'src/text/wf_width.c': ('LICENSES/unicode-data.txt',),
        'tools/make_ambiguous_width.py': ('LICENSES/unicode-data.txt',),
        'src/third_party/libarchive-3.8.10/libarchive/archive_string_composition.h':
            ('http://unicode.org/Public/18.0.0/ucd/UnicodeData.txt',
             'UNICODE LICENSE V3', 'Copyright © 1991-2026 Unicode, Inc.'),
        'src/third_party/libarchive-3.8.10/COPYING.GPLv2':
            ('GNU GENERAL PUBLIC LICENSE', 'Version 2, June 1991', 'END OF TERMS AND CONDITIONS'),
        'src/third_party/libarchive-3.8.10/COPYING.GPLv3':
            ('GNU GENERAL PUBLIC LICENSE', 'Version 3, 29 June 2007', 'END OF TERMS AND CONDITIONS'),
    }
    failures = []
    for name, markers in required.items():
        path = root / name
        if not path.is_file():
            failures.append(f'Missing additional notice: {name}')
        elif any(marker not in path.read_text(encoding='utf-8') for marker in markers):
            failures.append(f'Incomplete additional notice/reference: {name}')
    return failures


if __name__ == '__main__':
    raise SystemExit(main())
