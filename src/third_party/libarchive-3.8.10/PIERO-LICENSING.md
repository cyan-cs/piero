# Source-only build helpers

The `build/autoconf` helpers and embedded macros retain their individual
copyrights, GPL terms and exceptions. `COPYING.GPLv2` and `COPYING.GPLv3`
provide the license texts for these source files. These helpers are not
compiled, linked, executed or packaged by piero's Windows CMake build.
Their terms do not change the separate per-file terms of the libarchive
library sources. No exception is being removed or broadened here.

The license texts were copied verbatim from the local Git for Windows xz
license directory during the 2026-11-27 review; their canonical references are
https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt and
https://www.gnu.org/licenses/gpl-3.0.txt.

## Unicode table backport

On 2026-11-27, `libarchive/archive_string_composition.h` was updated using
both header changes from https://github.com/libarchive/libarchive/pull/3848:

- 29a2560c3d04427ac58cf7d9c8c4679dce19c9f8
- 6aafe269e2f34f3c0fd29caef35fa98bbdeeaa08

The downloaded two-commit patch SHA-256 was
`be2d6d141841a4082573c9f4a459165124818ae01d8b3ce4ac8020a08791d0a2`.

The pre-patch Git blob was 7f5b77a1d8a5ea2a0422b0f728c08d69f1471b1c.
The applied result matched upstream blob 3f1d6537e22408a0e1b6fb32bb754167af6e5b78.
Only its redundant final blank line was subsequently removed locally.
The table is derived from UnicodeData 18.0.0 and retains the upstream
Unicode License v3 and libarchive copyright notices. The corresponding
distribution notice is `LICENSES/libarchive-3.8.10.txt`.
The generation script was already excluded from this vendored subset and is
not introduced by this backport. The full upstream PR updates it as well.
Regression coverage in `tests/test_archive_unicode.c` exercises the linked
library's NFC/NFD conversions, including post-6.0 characters.

See [the licensing review](../../../docs/en/licensing.md).
