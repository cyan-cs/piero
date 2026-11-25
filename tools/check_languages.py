"""Check source catalogs or partial external language files without building."""
from pathlib import Path
import sys
from language_catalog import load, load_bundled

root = Path(__file__).resolve().parents[1]
base = load_bundled(root/'languages')[0]
reference = base['arguments']
paths = [Path(s) for s in sys.argv[1:]] or sorted((root/'languages').glob('*.ini'))
failed = False
for path in paths:
    try:
        language = load(path, reference, base['messages'])
        print('OK:', path.name, len(language['messages']), 'message keys')
    except (OSError, ValueError, UnicodeError):
        print('INVALID:', path.name, '(syntax, metadata, limits, encoding or placeholders)')
        failed = True
raise SystemExit(failed)
