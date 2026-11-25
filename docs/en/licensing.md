[English](../en/licensing.md) | [日本語](../ja/licensing.md)

# Licensing review

## Project license

Project-owned code and documentation use
the root [LICENSE](../../LICENSE), with the notice “2026 piero contributors”.
Preserve that copyright and permission notice when redistributing covered code.
Contributions to project-owned code use the same MIT terms. Only submit material
you have permission to contribute. This does not relicense third-party code or
establish ownership of material copied from elsewhere.

## Reading licenses in piero

In Settings, About contains a concise project page and a separate MIT License
entry. The project page ends with the sponsorship invitation and the owner's
X account, `https://x.com/dev_cyan`. It promises no benefits or development priority.
The repository is `https://github.com/cyan-cs/piero`.

The MIT entry and piero's entry under Licenses show the complete English legal
text, generated directly from the root `LICENSE`, including its copyright notice.
There is no translated or shortened legal substitute. CMake tracks changes to
that file and embeds the text, so it remains readable without a runtime sidecar.
Use Right to focus the text, Up/Down or Page Up/Page Down to scroll, Home/End
to reach the beginning/end, Left to return to entries and Esc to leave Settings.

Third-party entries read the notices in `LICENSES/`. Keep the root
license and those notices in distributions; embedding MIT does not remove them.

## Bundled sources and notices

| Component | Version | Source notice | Distribution notice |
| --- | --- | --- | --- |
| libarchive | 3.8.10 | `src/third_party/libarchive-3.8.10/COPYING` | `LICENSES/libarchive-3.8.10.txt` |
| zlib | 1.3.1 | `src/third_party/zlib-1.3.1/LICENSE` | `LICENSES/zlib-1.3.1.txt` |
| liblzma | 5.8.1 | `src/third_party/xz-5.8.1/COPYING.0BSD` | `LICENSES/xz-liblzma-5.8.1.txt` |
| zstd | 1.5.7 | `src/third_party/zstd-1.5.7/LICENSE` | `LICENSES/zstd-1.5.7.txt` |

libarchive has per-file BSD/public-domain notices and an aggregate COPYING;
zlib uses its zlib license; liblzma library uses 0BSD; zstd offers BSD/GPL
alternatives and the distribution carries its BSD notice. The vendored xz
tree also contains tools/build files with separate terms: do not describe the
entire tree as 0BSD. Full source publication must retain all upstream notices.
`cmake/product.cmake` copies the checkout's `LICENSES/` beside the product.

## Remaining checks

Review every compiled source's individual notice and source provenance before
distribution; a component inventory is not a complete legal audit. Confirm
the aggregate libarchive notice covers all selected files and preserve upstream
notices when updating. ASan/compiler-rt is development-only and must not enter
the normal package.

[MIT license text](https://opensource.org/license/mit)

## Tree-sitter and grammars

The Tree-sitter runtime (MIT) and the grammars piero builds into the optional `syntax`
folder (json, jsonc, yaml, toml, ini, xml, markdown, powershell, bash; MIT except ini, which is Apache-2.0) are
vendored in the source tree with their notices in `LICENSES/tree-sitter-0.27.2.txt` and
`LICENSES/tree-sitter-grammars.txt`. The queries in `grammar-pack` are written for piero.
Check the notice of a grammar again whenever its version is updated.
