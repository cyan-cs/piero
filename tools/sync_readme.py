"""Root README is canonical; rebase its relative links for docs/en/README.md."""
import argparse
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def expected():
    def rebase(match):
        label, target = match.groups()
        if ':' in target or target.startswith('#'):
            return match.group(0)
        path, sep, anchor = target.partition('#')
        new = os.path.relpath(ROOT / path, ROOT / 'docs/en').replace('\\', '/')
        return f'[{label}]({new}{sep}{anchor})'
    return re.sub(r'\[([^\]]+)\]\(([^\s)]+)\)', rebase, (ROOT / 'README.md').read_text(encoding='utf-8'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = ROOT / 'docs/en/README.md'
    if args.check:
        if not path.exists() or path.read_text(encoding='utf-8') != expected():
            raise SystemExit('English README differs; run python tools/sync_readme.py')
        print('English README synchronization: PASS')
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(expected(), encoding='utf-8', newline='\n')
