[English](../en/contributing.md) | [日本語](../ja/contributing.md)

# Contributing

## Before a contribution

This is a development preview with known limitations. Project-owned code and
contributions use MIT; submit only material you have permission to contribute
and preserve third-party notices. See [licensing](licensing.md) and [security](security.md).
Do not post sensitive vulnerability details or private test files publicly.

## Development workflow

Set up x64 Visual Studio C++/Windows SDK, clang-cl or MSVC, CMake and Ninja as
described in [development](development.md). Build/test both compilers in separate
directories. Format changed lines in C/header files using `.clang-format`.
Check explicit files with `clang-format --dry-run --Werror <explicit paths>`;
this check covers each whole file. Preserve vendor formatting.
Use [coding style](coding-style.md) and preserve ISO C17 and the documented safety invariants.

## Pull requests and commits

Describe the concrete problem, resulting behavior, tests and remaining limits.
One coherent purpose per commit; English commit messages. Add focused regression
tests for safety changes and distinguish mocks, Win32, ConPTY and real terminal
results. Fault/mutation tests need completed assertion failures, not infrastructure errors.
Preserve user edits and unrelated files. Never stage `.mcp.json`, credentials,
generated EXEs/DLLs, raw logs or private user documents. No push or release is
implied by local work. Update both language guides and i18n tables together.

## AI-assisted contributions

AI-assisted contributions are welcome. Contributors remain responsible for every
change they submit. Before opening a pull request, read and understand the changes,
build and test them, review safety and security risks, and check the project's
coding and architecture rules. Fix known defects before submission and disclose
untested behavior and remaining risks in the pull request.

AI tools do not replace human review or accountability. Maintainers may reject
contributions that have not been adequately reviewed or tested.

## Safety review

Changes to deletion, replacement, identity, ADS, recovery, worker lifetime or
AppContainer require explicit invariants and failure-path evidence. Do not
weaken checks to improve timing or test counts. Preserve originals on uncertainty.
Use disposable fixtures with ownership proof. Power-loss tests require an
owned disposable VM/disk; host drives and real user data are forbidden.

## Screenshots

### Files supplied by the owner

The README currently has no screenshot.
Use `assets/screenshots/` for `main.png`, `image-preview.png`, `pdf-preview.png`,
`editor.png`, `settings.png`.
Main screen: 16:9, about 1920×1080 or 1600×900. Detail views may use suitable
dimensions. Check both GitHub light and dark themes.

### Add and review

Capture the real application using disposable fixtures and no personal account,
user path, private filename, document content, token or credential on screen.
Inspect every image and its metadata; crop/redact before staging. Screenshots
are illustrations, not proof that a safety test passed.
After adding `main.png`, insert its Markdown below the introduction in root README and the corresponding location in Japanese README:

```markdown
![piero showing Miller columns](assets/screenshots/main.png)
```

For Japanese README use `../../assets/screenshots/main.png` and translated alt
text. Run `python tools/sync_readme.py` and `python tools/check_docs.py`.
Until the files exist, leave these examples in this guide; do not add broken
image references to README.
