[English](../en/development.md) | [日本語](../ja/development.md)

# Development

## Build from source

Use x64 Visual Studio Developer PowerShell with C++ tools and Windows SDK.
Install CMake 3.20+, Ninja, Python 3.10+ and clang-cl or MSVC.
Development targets Windows 11 x64. CI selects its compiler through
`tools/setup_ci_llvm.ps1`; this guide does not require a particular local patch version.
Keep the absolute build-directory path under 90 characters: some tests still use MAX_PATH.

From the repository root, build with clang-cl:

```powershell
cmake -S . -B out/dev -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Debug
cmake --build out/dev
ctest --test-dir out/dev --output-on-failure -L non-destructive
.\out\dev\piero.exe .
```

For MSVC, use a separate build directory and `-DCMAKE_C_COMPILER=cl`.
For product-only builds use `-DBUILD_TESTING=OFF`.
Do not run separate CTest invocations concurrently; filesystem fixtures share a named DOS-drive mutex.
The wrapper commands below also build and validate. They prefer Visual Studio's
bundled Ninja before PATH, which can help in restricted sessions.
PowerShell must permit local scripts under your organization's policy; otherwise
use the direct CMake commands above in a developer shell.

## Build and validation

Use separate short build directories for clang-cl, MSVC and sanitizer builds.
Product code is ISO C17; see the coding style and contribution guides. Vendored
libraries retain their own warning scope and notices. Downloading extra product
dependencies is not part of the normal build.

```powershell
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain clang -Configuration Debug -SafeOnly -BuildDirectory out/dev-clang
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain msvc -Configuration Release -SafeOnly -BuildDirectory out/dev-msvc
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain clang -Configuration Debug -Sanitize -SafeOnly -BuildDirectory out/dev-san
python tools/sync_readme.py
python tools/sync_doc_index.py
python tools/check_docs.py
```

The selected tests do not certify actual filesystem operations. Full filesystem,
interruption and volume trials require the owned disposable environment described
in manual validation. Never weaken an operation gate or convert failures into
skips to qualify a build. MSVC analysis is advisory and restricted to cl;
clang-cl is rejected for that option.

Sanitizers use the selected compiler's matching runtime. Protected non-Debug
parent startup uses a private assembly; copied decoder staging is still unqualified.
Do not ship sanitizer runtimes in normal packages. Real PDF isolation, terminal
behavior and storage durability require separate evidence.

Keep language pairs synchronized. The root English README is canonical; use the
README synchronizer after editing. The generated index lists current guides.
Use the [documentation entry point](../README.md) for guide maintenance and test procedures.
Keep new validation results in the ignored build directory, with the revision,
build configuration and environment needed to reproduce them.
Tests for update checking inject API/registry behavior without contacting the
network or changing the host PATH. Packaging is separate from publication.

## Benchmarks

With testing enabled, `bench_core` measures fixed workloads after warmup and writes
`summary.json` and `samples.csv` into a new directory. Existing output directories
are refused. Results include P50/P95/P99, emitted bytes and process resource counts;
there are no CI performance thresholds.

```powershell
cmake --build out/dev-msvc --target bench_core
out/dev-msvc/bench_core.exe --output out/bench-first --samples 101
python tests/test_benchmark.py out/dev-msvc/bench_core.exe
```

The measurements cover CPU frame construction, input-to-frame processing, VT
generation, resizing, Sixel and directory models/sorting. They exclude terminal
painting and actual console write calls. Add `--enumerate` with an existing owned
fixture directory for read-only Win32 enumeration; fixture creation is not timed.
User language overlays are excluded from the fixed workload. Process memory
retention is not proof of a leak. Compare matching builds, inputs and environments.

## Decoder and sanitizer diagnostics

Distinguish AppContainer profile creation from process startup and decoding.
[probe_appcontainer.c](../../tools/probe_appcontainer.c) tests profile creation;
[probe_decoder_start.c](../../tools/probe_decoder_start.c) exercises the product's
decoder startup. A derived SID does not prove that profile creation succeeded,
and an HRESULT alone does not identify the missing resource. Compare a normal
user session with the failing environment; keep raw identity and profile traces private.

Non-Debug builds use `/DEPENDENTLOADFLAG:0x800`, so adding compiler-rt to PATH
does not resolve a protected executable's static imports. Sanitized parent
startup uses the selected runtime's private assembly, `piero.asan.runtime`.
This does not qualify the separately staged decoder. Do not put ASan in System32
or add it to ordinary distribution packages. Record parent startup and decoder startup results separately, including the
compiler version and runtime used.

## Session preservation

`src/session/wf_session_store.c` validates saved data after acquiring the writer
lock, including during persistence-only startup. Damaged, unreadable or unsupported
data blocks checkpoints; only successfully emptying both slots clears that block.
A whole supported slot can recover a torn companion through the normal slot reader.

`tests/test_main_session.c` runs `wf_main_session_start`, a checkpoint and shutdown
from `src/main_session.c` against disposable session files and fake navigation.
It checks byte preservation with and without restoration, unreadable headers,
unsupported versions and saving after an explicit clear. `tests/test_session.c`
also checks that a failed clear does not allow later overwrites.

`tests/test_session_alloc.c` fails each codec allocation and checks complete
snapshots or `WF_E_NOMEM`, empty failed outputs and cleanup of partial records.
It also checks that a failed duplicate-cache update forgets the old snapshot.
