# Language catalogs

The editable sources are `languages/en.ini` and `languages/ja.ini`.
Message IDs and the emergency English catalog are generated from INI at build
time. Runtime translations are loaded as bounded, untrusted data; no translation
table is maintained by hand in C.

See the [English guide](../../docs/en/translating.md) or
[日本語ガイド](../../docs/ja/translating.md) for file syntax, discovery, overrides,
limits, fallback, packaging and validation.
