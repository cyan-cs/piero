"""Read-only commit checks shared by Linux and Windows CI; no fetch or Git writes."""
import argparse
import json
import os
import re
import subprocess
from pathlib import Path


def git(*args, data=None, check=True):
    result = subprocess.run(['git', *args], input=data, capture_output=True)
    if check and result.returncode:
        # Git diagnostics/diffs can include source contents or private paths.
        raise RuntimeError('Git comparison command failed; diagnostic contents withheld')
    return result


def comparison(event_name, event):
    head = git('rev-parse', '--verify', 'HEAD^{commit}').stdout.decode().strip()
    base = None
    reason = 'full tree: no usable comparison base'
    if event_name == 'pull_request':
        base = event.get('pull_request', {}).get('base', {}).get('sha')
    elif event_name == 'push':
        base = event.get('before')
    # Dispatch deliberately audits the entire selected tree, not just HEAD's parent.
    if isinstance(base, str) and re.fullmatch(r'[0-9a-fA-F]{40}|[0-9a-fA-F]{64}', base) and set(base) != {'0'}:
        if git('cat-file', '-e', base + '^{commit}', check=False).returncode == 0:
            if event_name == 'pull_request':
                merged = git('merge-base', base, head, check=False)
                base = merged.stdout.decode().strip() if merged.returncode == 0 else None
            if base:
                return base, head, 'commit range: ' + event_name
    # Git recognizes the canonical empty tree even in a new repository. Compute
    # it for this repository's object format without writing an object/index/ref.
    empty = git('hash-object', '-t', 'tree', '--stdin', data=b'').stdout.decode().strip()
    git('cat-file', '-e', empty + '^{tree}')
    return empty, head, reason


def changed_paths(base, head):
    raw = git('diff', '--no-ext-diff', '--no-textconv', '--no-renames',
              '--name-only', '-z', '--diff-filter=ACMT', base, head, '--').stdout
    return [os.fsdecode(name) for name in raw.split(b'\0') if name]


def format_paths(base, head):
    # Intersect changed entries with the final tree. Deleted entries and vendor
    # code never reach the formatter; NUL delimiters preserve unusual names.
    changed = set(changed_paths(base, head))
    paths = []
    for entry in git('ls-tree', '-r', '-z', head).stdout.split(b'\0'):
        if not entry:
            continue
        info, raw = entry.split(b'\t', 1)
        name = os.fsdecode(raw)
        if (name in changed and name.startswith(('src/', 'tests/', 'tools/')) and
                not name.startswith('src/third_party/') and Path(name).suffix.lower() in ('.c', '.h')):
            if info.split()[0] not in (b'100644', b'100755'):
                raise RuntimeError('Selected C/header entry is not a regular file')
            paths.append(name)
    return paths


def whitespace(base, head):
    # Preserve upstream formatting, as in the C/header formatting check.
    paths = ('.', ':(top,exclude)src/third_party/**')
    result = git('-c', 'core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol',
                 'diff', '--no-ext-diff', '--no-textconv', '--check', base, head, '--', *paths, check=False)
    if result.returncode:
        raise RuntimeError('Committed whitespace check failed; source lines withheld. Run git diff --check locally for details.')
    count = len(git('diff', '--no-ext-diff', '--no-textconv', '--name-only', '-z', base, head, '--', *paths).stdout.split(b'\0')) - 1
    print(f'Committed whitespace: PASS ({count} changed paths inspected)')


def formatting(base, head):
    llvm = os.environ.get('PIERO_LLVM_ROOT')
    if not llvm:
        raise RuntimeError('PIERO_LLVM_ROOT must select the verified CI LLVM installation')
    executable = Path(llvm) / 'bin' / 'clang-format.exe'
    if not executable.is_file():
        raise RuntimeError('Selected clang-format executable is missing')
    root = Path.cwd().resolve()
    paths = format_paths(base, head)
    for name in paths:
        path = root / name
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise RuntimeError('Selected C/header file is missing or outside the checkout')
        result = subprocess.run([str(executable), '--dry-run', '--Werror', '--', name], capture_output=True)
        if result.returncode:
            raise RuntimeError('clang-format check failed; source diagnostics withheld. Run the formatter locally for details.')
    print(f'clang-format: PASS ({len(paths)} managed C/header files inspected)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--whitespace', action='store_true')
    parser.add_argument('--format', action='store_true')
    args = parser.parse_args()
    if not args.whitespace and not args.format:
        parser.error('Select --whitespace and/or --format')
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
    if not isinstance(event, dict):
        raise ValueError('Event must be an object')
    base, head, reason = comparison(os.environ['GITHUB_EVENT_NAME'], event)
    print(f'Comparison: {reason}; {base} -> {head}')
    if args.whitespace:
        whitespace(base, head)
    if args.format:
        formatting(base, head)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        print('CI comparison/format check: FAIL; ' + str(error))
        raise SystemExit(1)
    except (OSError, ValueError, KeyError):
        # No exception text from JSON, paths, source files or child processes.
        print('CI comparison/format check: FAIL; inspect prerequisites or run the committed checks locally')
        raise SystemExit(1)
