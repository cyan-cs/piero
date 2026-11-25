# Translations

[日本語](../ja/translating.md)

Edit UTF-8 INI files without rebuilding. Start with `languages/en.ini` or
`languages/ja.ini`, rename the copy to `fr.ini`, and change `[language]` `code`
to `fr` and `name` to its native display name. Codes are lowercase ASCII letters,
digits and hyphen-separated subtags, at most 31 bytes, starting with a letter.
The filename must match the code. `auto` is reserved. Names are at most 128 UTF-8
bytes; the picker also shows the code to distinguish identical names.

Place files in either `languages/` beside the executable or
`%LOCALAPPDATA%\piero\languages\`. The user directory overrides bundled values
for the same code. Partial files inherit bundled translations; keys absent from
both layers use the generated emergency English text. Each file needs `code` and
`name`. `plural` is optional: `one-other` means singular for 1; `none` always uses
the other form. A partial override without `plural` keeps the bundled rule.
The existing two-form API does not implement arbitrary plural rules.

```ini
[language]
code = fr
name = Français
plural = one-other

[messages]
MSG_SETTINGS_TITLE = Paramètres
```

Open Settings → General → Language, select a language and save with Ctrl+S.
Discovery runs at startup and when opening settings; selecting a language reloads
its files. Normal file operations do not poll translation files. Within a session,
a removed language retains its code and falls back to English. On a subsequent
start, a missing configured language is marked unavailable and displays English
without preventing startup. Its code is preserved; selecting English explicitly
can still save a different code.
Language-folder opening is not a menu action; use the paths above.

LF and CRLF, an optional UTF-8 BOM, and whole-line `;`/`#` comments are accepted.
The first `=` separates a key and value; further `=`, `;` and `#` are literal.
Leading/trailing spaces are trimmed; use `\s` to preserve them. `\\` is a literal
backslash and `\n` a line break (multiline viewers support it; single-line UI
fields show it as `?`). Other escapes are invalid. Duplicate keys in one section
reject the whole file. Unknown keys are ignored; malformed syntax, invalid UTF-8,
NUL, terminal controls, invalid placeholders or metadata reject the file. A bad
user file leaves the bundled layer or emergency English available.

Keep `{0}`…`{3}` identifiers and required argument counts unchanged. `{{` is a
literal opening brace. Substitution types come from the caller's string/integer
arguments; translations cannot introduce conversion/type specifiers. Text is never
a printf format; `%n` conversions are rejected. `[arguments]` in `en.ini` defines
build-time arities; other files may omit it, but cannot change those arities.
Do not rename existing `MSG_*` keys. Append new program message keys to English,
add their counts, and translate them in Japanese. Adding a language needs no C or
CMake edit. INI is the only editable catalog; generated headers are disposable.

Limits: 32 languages including English/Japanese, 64 candidate filenames per
directory, 1 MiB per file, 8,192 key assignments, 4,096 UTF-8 bytes per decoded
value. Symlink/reparse-point files are refused. Reading denies concurrent
write/delete sharing and checks size/timestamps; an open writer causes fallback.
Returned strings remain valid across switches in a deduplicating 16 MiB session
pool; exhausting it causes fallback rather than unbounded retention or dangling
pointers. Restart to reclaim that pool. Translation text never controls file
operation gates, identities, hashes or recovery decisions.

Developer builds use Python 3.10+ for the included tooling and generate IDs
and English fallback from INI.
If CMake finds an unusable interpreter shim, pass
`-DPython3_EXECUTABLE=C:/path/to/python.exe`. Optional validation:

```powershell
python tools/check_languages.py
python tools/check_languages.py path/to/fr.ini
```

Both ZIP and installer payloads contain `languages/en.ini` and `languages/ja.ini`.
Keep that directory with a relocated executable. User translations under
`%LOCALAPPDATA%\piero\languages\` are not installed, updated or removed by the
package. Runtime use does not require Python.
