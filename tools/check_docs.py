"""Check all current guides, language pairs and local references without dependencies."""
import collections
import functools
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
errors = []


def body(path):
    raw = path.read_bytes()
    if b'\r' in raw or not raw.endswith(b'\n'):
        errors.append(f'{path.relative_to(ROOT)}: use LF with final newline')
    text = raw.decode('utf-8-sig', errors='strict')
    if re.search(r'\?{3,}', text):
        errors.append(f'{path.relative_to(ROOT)}: suspicious encoding replacement question marks')
    fence, previous = False, 0
    for i, line in enumerate(text.splitlines(), 1):
        if line.startswith('```'):
            fence = not fence
        if not fence and (match := re.match(r'^(#+) ', line)):
            level = len(match[1])
            if previous and level > previous + 1:
                errors.append(f'{path.relative_to(ROOT)}:{i}: heading level jumps')
            previous = level
        if line.rstrip() != line:
            errors.append(f'{path.relative_to(ROOT)}:{i}: trailing whitespace')
    if fence:
        errors.append(f'{path.relative_to(ROOT)}: unclosed code fence')
    return text


def structure(text):
    return [len(m[1]) for m in re.finditer(r'^(#+) ', text, re.M)]


def examples(text):
    return re.findall(r'^```([^\n]*)\n(.*?)^```', text, re.S | re.M)


def prose(text):
    return re.sub(r'^```[^\n]*\n.*?^```', '', text, flags=re.S | re.M)


def inline(text):
    return collections.Counter(re.findall(r'(?<!`)`([^`\n]+)`(?!`)', prose(text)))


en = {p.name: p for p in (ROOT / 'docs/en').glob('*.md')}
ja = {p.name: p for p in (ROOT / 'docs/ja').glob('*.md')}
if en.keys() != ja.keys():
    errors.append(f'Unpaired files: {sorted(en.keys() ^ ja.keys())}')
paths = [ROOT / name for name in ('README.md', 'CONTRIBUTING.md', 'SECURITY.md', 'CODING_STYLE.md')]
paths += list(en.values()) + list(ja.values()) + list((ROOT / 'assets').rglob('*.md'))
paths += list((ROOT / 'docs').glob('*.md'))
texts = {p: body(p) for p in paths}
for name in en.keys() & ja.keys():
    a, b = texts[en[name]], texts[ja[name]]
    if structure(a) != structure(b):
        errors.append(f'{name}: paired heading structure differs')
    if examples(a) != examples(b):
        errors.append(f'{name}: paired code examples differ')
    if inline(a) != inline(b):
        errors.append(f'{name}: inline tokens differ: EN {dict(inline(a)-inline(b))}, JA {dict(inline(b)-inline(a))}')
    def numbers(text):
        text = re.sub(r'\[[^\]]*\]\([^)]*\)', '', prose(text))
        return collections.Counter(re.findall(r'(?<![A-Za-z0-9_])\d+(?:\.\d+)*(?![A-Za-z0-9_])', text))
    if numbers(a) != numbers(b):
        errors.append(f'{name}: numeric facts differ: EN {dict(numbers(a)-numbers(b))}, JA {dict(numbers(b)-numbers(a))}')
# Check links in other tracked Markdown without imposing guide layout rules.
link_texts = dict(texts)
inventory = set(subprocess.check_output(
    ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
    cwd=ROOT).decode('utf-8').split('\0'))
for name in inventory:
    p = ROOT / name
    if (name.endswith('.md') and p.is_file() and p not in link_texts and
            (name.startswith(('docs/', 'readme/', 'assets/')) or '/' not in name)):
        link_texts[p] = p.read_text(encoding='utf-8-sig')


@functools.lru_cache(maxsize=None)
def child_names(parent):
    return {p.name for p in parent.iterdir()}


def exact_case(path):
    parent = ROOT
    for part in path.relative_to(ROOT).parts:
        if part not in child_names(parent):
            return False
        parent /= part
    return True


@functools.lru_cache(maxsize=None)
def anchors(path):
    """GitHub-style heading IDs, including suffixes for repeated headings."""
    found, counts = set(), collections.Counter()
    text = prose(path.read_text(encoding='utf-8-sig'))
    for heading in re.findall(r'^#+\s+(.+?)\s*#*$', text, re.M):
        heading = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', heading)
        heading = re.sub(r'<[^>]+>', '', heading).lower()
        slug = ''.join(c for c in heading if c.isalnum() or c in '_- ')
        slug = slug.replace(' ', '-')
        count = counts[slug]
        found.add(f'{slug}-{count}' if count else slug)
        counts[slug] += 1
    found.update(re.findall(r'<a\s+(?:id|name)=[\"\']([^\"\']+)', text))
    return found


for p, text in link_texts.items():
    for target in re.findall(r'!?\[[^\]]*\]\(([^\s)]+)\)', prose(text)):
        if ':' in target:
            continue
        target, _, anchor = target.partition('#')
        target, anchor = unquote(target), unquote(anchor)
        # resolve() on Windows can recover the on-disk spelling. Check the
        # original normalized spelling as well, or a wrong-case link can pass.
        logical = Path(os.path.abspath(p.parent / target)) if target else p
        dest = logical.resolve()
        if not dest.is_relative_to(ROOT) or not dest.exists():
            errors.append(f'{p.relative_to(ROOT)}: missing/unsafe link {target}')
        elif not logical.is_relative_to(ROOT) or not exact_case(logical):
            errors.append(f'{p.relative_to(ROOT)}: link case differs from disk {target}')
        elif dest.is_file():
            rel = dest.relative_to(ROOT).as_posix()
            if rel not in inventory:
                errors.append(f'{p.relative_to(ROOT)}: linked file is ignored {rel}')
            if anchor and dest.suffix == '.md' and anchor not in anchors(dest):
                errors.append(f'{p.relative_to(ROOT)}: missing heading #{anchor} in {rel}')
    if p in texts:
        # Concrete source paths in prose must survive renames/deletions too.
        # Examples and placeholders describe user data rather than repo files.
        for token in inline(text):
            if (re.match(r'^(?:src|tests|tools|cmake|packaging|languages|LICENSES|grammar-pack|\.github)/', token)
                    and not re.search(r'[\s<>*|]', token)):
                logical = Path(os.path.abspath(ROOT / token))
                if not logical.is_relative_to(ROOT) or not logical.exists() or not exact_case(logical):
                    errors.append(f'{p.relative_to(ROOT)}: missing/mis-cased source reference {token}')
subprocess.run([sys.executable, str(ROOT / 'tools/sync_doc_index.py'), '--check'], check=True)
subprocess.run([sys.executable, str(ROOT / 'tools/sync_readme.py'), '--check'], check=True)
if errors:
    print('\n'.join(errors))
    raise SystemExit(1)
print(f'Public documentation: PASS ({len(en)} language pairs, {len(paths)} current files, {len(link_texts)} linked Markdown files); semantic translation review still required')
