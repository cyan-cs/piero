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
alternatives and the distribution carries its BSD notice. The upstream xz
distribution also contains tools/build files with separate terms: do not describe the
entire tree as 0BSD. Full source publication must retain all upstream notices.
`cmake/product.cmake` copies the checkout's `LICENSES/` beside the product.

## Notice coverage checks

The distribution notices also reproduce the individual copyright and license
comments in the compiled vendor sources and headers, including inline borrowed
code inside libarchive. Tree-sitter's notice includes the complete license of
its bundled ICU/Unicode headers. Zstandard uses its BSD license option.

After building, run:

```powershell
python tools/audit_licenses.py out/cr --output out/license-review.json --check-notices
```

The `license_notices` CTest runs the same check. It follows Ninja's actual header
dependencies as well as its C translation units, checks the upstream license
texts, and rejects unknown vendor components. License-only changes refresh the
notices beside an existing product build. ZIP and installer payloads retain the
notices without changes. ASan/compiler-rt is development-only and must not enter
the normal package.

This is a check of notice coverage, not proof of ownership, source provenance,
or every upstream licensing condition. Review changed terms and provenance when
updating dependencies; copying a new comment into a notice is not legal review.

## Unicode data finding resolved (2026-11-27)

The original libarchive 3.8.10 `archive_string_composition.h` was generated
from UnicodeData 6.0.0 and carried only the libarchive notice. Upstream identified
the missing Unicode notice in [issue #3817](https://github.com/libarchive/libarchive/issues/3817)
and resolved it by updating the data and adding its license in
[PR #3848](https://github.com/libarchive/libarchive/pull/3848). Both header changes
have now been backported: the resulting Git blob matched the upstream patch,
then only the redundant final blank line was removed. The table now uses
UnicodeData 18.0.0 and includes Unicode License v3. Its entire notice is also
included in `LICENSES/libarchive-3.8.10.txt` for binary distribution.

The 2026-11-27 backport provenance is recorded in the vendor directory's
`PIERO-LICENSING.md`. The `archive_unicode` regression exercises actual linked
NFC/NFD conversion, including post-6.0 characters, Japanese, combining marks,
Hangul and composition exclusions. The notice check also rejects a rollback to
the old data without its Unicode notice. This closes the known missing-notice
finding; automatic checks still cannot discover terms absent from upstream.

[MIT license text](https://opensource.org/license/mit)

## Additional data and source-distribution review

The Unicode 15.0.0 width ranges in `src/text/wf_width.c` and
`src/text/wf_width_ambiguous.h` are modified representations of character data,
including data queried through Python's `unicodedata`; they do not contain Python
interpreter code. `LICENSES/unicode-data.txt` retains the data notice, permission
text and source references. The generator preserves a link to that notice.

The source checkout also contains libarchive's GNU Autoconf/Libtool helpers.
Their GPL notices and individual exceptions remain in the source; the complete
GPL texts are supplied in that vendor directory. They are not built into or
packaged with the Windows product. Do not identify the entire source checkout as
MIT or describe every libarchive file as BSD. The root MIT applies to
project-owned material only.

The portable package imports only reviewed Windows system DLLs and uses the
static CRT. Python, LLVM, CMake and Ninja are build tools, not bundled programs.
An installer additionally contains the Inno Setup engine: retain its existing
copyright notices and website addresses as required by the
[Inno Setup license](https://jrsoftware.org/files/is/license.txt). This environment
has no installer compiler, so an actual installer binary has not been checked.

## Grammar sources

The Tree-sitter runtime (MIT) and the grammars piero builds into the optional `syntax`
folder (json, jsonc, yaml, toml, ini, xml, markdown, powershell, bash; MIT except ini, which is Apache-2.0) are
vendored in the source tree with their notices in `LICENSES/tree-sitter-0.27.2.txt` and
`LICENSES/tree-sitter-grammars.txt`. The queries in `grammar-pack` are written for piero.
Check the notice of a grammar again whenever its version is updated.
