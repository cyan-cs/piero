"""Bounded UTF-8 language INI validation shared by generation and packaging."""
from pathlib import Path
import re
import hashlib

FILE_LIMIT = 1 << 20
TEXT_LIMIT = 4096
KEY_LIMIT = 8192
CODE = re.compile(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*\Z')

def safe(text, multiline=False):
    if len(text.encode('utf-8')) > TEXT_LIMIT:
        raise ValueError('Text too long')
    for c in text:
        n = ord(c)
        if (n < 32 and not (multiline and c == '\n')) or 127 <= n <= 159 or 0x202a <= n <= 0x202e or 0x2066 <= n <= 0x2069:
            raise ValueError('Control character')
    if '%n' in text or re.search(r'%[0-9$.*+ #hljztLI-]*n', text):
        raise ValueError('Forbidden printf conversion')

def unescape(text):
    result, i = [], 0
    while i < len(text):
        if text[i] != '\\':
            result.append(text[i])
        else:
            i += 1
            if i == len(text) or text[i] not in {'\\', 'n', 's'}:
                raise ValueError('Unknown escape')
            result.append({'\\': '\\', 'n': '\n', 's': ' '}[text[i]])
        i += 1
    value = ''.join(result)
    safe(value, multiline=True)
    return value

def signature(text):
    counts, i = [0,0,0,0], 0
    while i < len(text):
        if text[i:i+2] == '{{':
            i += 2
        elif text[i] == '{':
            if i+2 >= len(text) or text[i+1] not in '0123' or text[i+2] != '}':
                raise ValueError('Invalid placeholder')
            counts[int(text[i+1])] += 1
            i += 3
        elif text[i] == '}':
            raise ValueError('Unmatched brace')
        else:
            i += 1
    return tuple(counts)

def mask(text):
    return sum(1 << i for i,n in enumerate(signature(text)) if n)

def load(path, reference=None, templates=None):
    path = Path(path)
    with path.open('rb') as stream:
        raw = stream.read(FILE_LIMIT + 1)
    if len(raw) > FILE_LIMIT:
        raise ValueError('Language file too large')
    text = raw.decode('utf-8-sig')
    for c in text:
        if ((ord(c) < 32 and c not in '\r\n\t') or 127 <= ord(c) <= 159
                or 0x202a <= ord(c) <= 0x202e or 0x2066 <= ord(c) <= 0x2069):
            raise ValueError('Invalid document control')
    sections, seen, section = {}, set(), None
    for line in text.split('\n'):
        if line.endswith('\r'):
            line = line[:-1]
        line = line.strip(' \t')
        if not line or line[0] in '#;':
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            if not re.fullmatch('[A-Za-z_][A-Za-z_0-9.-]{0,63}', section):
                raise ValueError('Invalid section')
            sections.setdefault(section, {})
            continue
        if section is None or '=' not in line:
            raise ValueError('Malformed INI')
        key, value = line.split('=', 1)
        key = key.strip(' \t')
        if not re.fullmatch('[A-Za-z_][A-Za-z_0-9.-]{0,95}', key):
            raise ValueError('Invalid key')
        if (section, key) in seen or len(seen) >= KEY_LIMIT:
            raise ValueError('Duplicate or too many keys')
        seen.add((section, key))
        sections[section][key] = unescape(value.strip(' \t'))
    language = sections.get('language', {})
    code, name = language.get('code', ''), language.get('name', '')
    if not 1 <= len(code) <= 31 or not CODE.fullmatch(code) or code == 'auto' or code != path.stem.lower():
        raise ValueError('Invalid language code/filename')
    safe(name)
    if not name or len(name.encode('utf-8')) > 128:
        raise ValueError('Missing/invalid language name')
    plural = language.get('plural', 'one-other')
    if plural not in ('none', 'one-other'):
        raise ValueError('Unsupported plural rule')
    messages = sections.get('messages', {})
    arguments = sections.get('arguments', {})
    arities = {}
    for key, value in arguments.items():
        if not re.fullmatch('[0-4]', value):
            raise ValueError('Invalid argument count')
        arities[key] = int(value)
    if reference is None:
        if not messages or set(messages) != set(arities):
            raise ValueError('English requires all argument counts')
        reference = arities
    for key, value in messages.items():
        if key in reference and (not value or mask(value) != (1 << reference[key])-1):
            raise ValueError('Missing/invalid placeholders: ' + key)
        if templates is not None and key in templates and signature(value) != signature(templates[key]):
            raise ValueError('Placeholder occurrences disagree: ' + key)
    for key, value in arities.items():
        if key in reference and value != reference[key]:
            raise ValueError('Argument metadata disagrees: ' + key)
    return dict(code=code, name=name, plural=plural, messages=messages, arguments=arities,
                source_sha256=hashlib.sha256(raw).hexdigest())

def load_bundled(directory):
    directory = Path(directory)
    en = load(directory / 'en.ini')
    ja = load(directory / 'ja.ini', en['arguments'], en['messages'])
    if en['code'] != 'en' or ja['code'] != 'ja' or set(en['messages']) != set(ja['messages']):
        raise ValueError('Bundled English/Japanese must be complete')
    return en, ja
