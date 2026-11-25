"""Generate the paired index from the current guide filenames and titles."""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GROUPS = (
    ('Using piero', '使い方', (
        'README.md', 'getting-started.md', 'features.md', 'keybindings.md',
        'configuration.md', 'syntax.md', 'updates.md', 'safety.md',
        'troubleshooting.md',
    )),
    ('Development and validation', '開発と検証', (
        'contributing.md', 'development.md', 'architecture.md', 'coding-style.md',
        'manual-validation.md', 'manual-verification-procedures.md', 'ci.md',
        'packaging.md', 'translating.md',
    )),
    ('Project information', 'プロジェクトについて', (
        'security.md', 'licensing.md', 'sponsorship.md',
    )),
)


def expected(language):
    files = {p.name for p in (ROOT / 'docs/en').glob('*.md') if p.name != 'index.md'}
    grouped = [name for _, _, names in GROUPS for name in names]
    if set(grouped) != files or len(grouped) != len(set(grouped)):
        raise SystemExit('Update GROUPS in tools/sync_doc_index.py to cover each guide once')
    sections = []
    for en_title, ja_title, names in GROUPS:
        rows = []
        for name in names:
            titles = []
            for lang in ('en', 'ja'):
                text = (ROOT / f'docs/{lang}' / name).read_text(encoding='utf-8')
                titles.append(re.search(r'^# (.+)$', text, re.M)[1])
            rows.append(f'| [{titles[0]}](../en/{name}) | [{titles[1]}](../ja/{name}) |')
        heading = en_title if language == 'en' else ja_title
        sections.append(f'## {heading}\n\n| English | 日本語 |\n| --- | --- |\n' + '\n'.join(rows))
    title = 'Documentation' if language == 'en' else 'ドキュメント一覧'
    intro = ('Start with getting started, features, keybindings and configuration. '
             'Development, packaging and CI are for contributors.'
             if language == 'en' else
             '最初は導入・機能・キー操作・設定を参照してください。'
             '開発・パッケージ作成・CIは貢献者向けです。')
    return (f'[English](../en/index.md) | [日本語](../ja/index.md)\n\n# {title}\n\n'
            f'{intro}\n\n' + '\n\n'.join(sections) +
            '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for language in ('en', 'ja'):
        path = ROOT / f'docs/{language}/index.md'
        content = expected(language)
        if args.check:
            if not path.is_file() or path.read_text(encoding='utf-8') != content:
                raise SystemExit('Documentation index differs; run python tools/sync_doc_index.py')
        else:
            path.write_text(content, encoding='utf-8', newline='\n')
    print('Documentation index: PASS' if args.check else 'Documentation index: updated')
