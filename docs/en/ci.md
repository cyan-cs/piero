[English](../en/ci.md) | [日本語](../ja/ci.md)

# GitHub Actions validation

## Scope

The workflow in `.github/workflows/validation.yml` checks pushes, pull requests
and manual dispatches. It performs no authenticated repository write and retains
`contents: read` only. It does not publish packages or releases.
CI success does not certify AppContainer, filesystem or real-terminal safety.

Before publishing source, inspect the selected files, including hidden files.
Exclude local agent/cloud settings, credentials, environment files, generated
output and raw evidence. Ignore rules do not remove files already tracked.
Review Git history separately; a clean working tree does not prove it contains no secrets.
The secret guard is a high-confidence pattern/name check, not a complete audit.

## Pinned actions and checkout

Official release pages link each version to the full commit below. Major versions
are retained; checkout includes the later authentication-cleanup patch.

| Action | Version | Commit |
| --- | --- | --- |
| actions/checkout | v4.3.1 | `34e114876b0b11c390a56381ad16ebd13914f8d5` |
| actions/setup-python | v5.6.0 | `a26af69be951a213d495a4c3e4e4022e16d87065` |
| actions/upload-artifact | v4.6.2 | `ea165f8d65b6e75b540449e92b4886f43607fa02` |
| actions/cache (restore/save) | v5.0.3 | `cdf6c1fa76f9f475f3d7449005a359c84ca0f306` |
| Mozilla-Actions/sccache-action | v0.0.11 | `fc920bf0ec8de6ee65d409111f7ec508035751ba` |

Sources: [checkout release](https://github.com/actions/checkout/releases/tag/v4.3.1),
[Python release](https://github.com/actions/setup-python/releases/tag/v5.6.0),
[artifact release](https://github.com/actions/upload-artifact/releases/tag/v4.6.2).
Cache sources: [cache release](https://github.com/actions/cache/releases/tag/v5.0.3),
[sccache action release](https://github.com/Mozilla-Actions/sccache-action/releases/tag/v0.0.11).
All checkouts set `persist-credentials: false`. Only documentation/history scanning
and formatting fetch full history with `fetch-depth: 0`; compiler/sanitizer/analysis
jobs use the default shallow checkout. No extra token permission is needed for
the test-artifact service. No PR creation or approval is performed.

## Committed checks

`tools/ci_changes.py` reads event JSON as data rather than injecting event values
into shell source. It always compares the checked-out `HEAD` tree. Push uses a
verified existing before commit; PR uses the merge base with the event's base
commit, including the normal synthetic-merge checkout. Missing/zero/invalid bases,
unrelated histories and manual dispatch compare the whole tree to the empty tree.
Failure to determine or inspect a tree is a failure, never an unconditional success.
Whitespace checking inspects the final tree difference, not only the clean worktree.
The reported path count distinguishes a completed empty difference from no check.
CRLF is accepted as a line ending; spaces before it still fail. Whitespace and
formatting checks both exclude `src/third_party/` to preserve upstream files.
The root `LICENSE` is checked out with LF so its embedded legal text matches
the runtime file byte for byte, regardless of the user's checkout preferences.

Formatting uses the same comparison, NUL-delimited filenames and argument arrays.
It checks final regular C/header files under source/tests/tools, excludes
`src/third_party/`, and omits deleted files. A full-tree fallback checks every
managed C/header file. Formatter failures fail the job. No source lines from Git
or formatter diagnostics are echoed by this helper; reproduce failures locally.

`tools/check_secrets.py --history` also scans current tracked/unignored contents,
rejects tracked local settings/key files/generated binaries, and refuses a shallow
history scan. Historical findings identify objects; matched values are not printed.
Do not rewrite project history as part of routine validation.

## Execution and artifacts

The local `setup-ci-llvm` action shares the compressed LLVM release archive
between jobs and runs. The key hashes the provisioning/download helpers; every
restore still checks the pinned SHA-256 before extraction and checks tool
versions afterwards. Only the approximately 264 MiB archive is cached, keeping
the 4 GiB installation out of cache storage. It is saved immediately after
verification, even if later tests fail. Extraction remains per job, at a stable
compiler path so compiler-cache entries can be reused.

The compiler matrix and sanitizer jobs use sccache v0.16.0 with its GitHub Actions storage backend via
`-CompilerCache`. CMake uses the compiler launcher and embedded `/Z7` Debug
information instead of a shared compiler PDB. Release mutation builds compile
one source per cached invocation, including their unchanged test drivers, then
link a fresh executable. Other direct-compiler mutation helpers retain their
existing build paths. The sccache action reports hits/misses in its post step.
See the [sccache usage guide](https://github.com/mozilla/sccache#usage).

Cached compilation never reuses a control/mutant verdict, CTest report or test
fixture. All tests still execute on every run. Cold caches still compile; the
actual warm-run saving must be measured in Actions. Long E2E and mutation
execution times remain, so caching alone does not promise a short full suite.
Local validation defaults to compilation without a cache; `-CompilerCache`
requires an installed sccache and also enables it for CTest's mutation builds.

Push validation stays enabled for development branches as well as the default
branch. Automatic runs supersede the same event/ref; PR refs stay separate.
Manual dispatch uses a unique run identifier, so an automatic push or another
manual analysis run does not cancel it through the concurrency group.

Both compilers retain Debug/Release builds and full CTest; Debug ASan/UBSan,
formatting, documentation, secrets and optional MSVC analysis remain present.
No known failure becomes a warning or ignored exit. Hosted-runner and
AppContainer behavior remain unverified until the workflow actually runs.

The validation helper's optional `-JUnit` writes local raw CTest XML. The collector
exports only test identifiers, verdicts, durations, counts and a blocked release
status. It never exports output, failure messages, commands, properties or paths.
Absent/invalid/empty reports fail collection and cannot masquerade as success.
Only `out/ci/ci-results.json` or `out/san/ci-results.json` is uploaded, including
failure summaries. Names distinguish compiler/configuration and run attempt;
retention is 7 days, hidden files are excluded and missing uploads fail.
Cancellation or runner loss can still prevent artifact delivery. Raw diagnostics
stay out of artifacts; review Actions logs privately before sharing them.

## Verification and owner checks

```powershell
python -m unittest discover -s tests -p test_ci_tools.py -v
python -m unittest discover -s tests -p test_docs.py -v
python tools/check_docs.py
```

The tests construct synthetic repositories without reusing the original Git metadata.
Confirm the repository Action policy, token permissions, log access and
source selection before publishing. Inspect every job on the first run;
workflow acceptance is not release qualification.

## Process-control prerequisites

`lock_policy` checks synthetic policy facts. `lock` additionally needs readable
process ancestry and an interactive session. Its owned helper probe reports
BLOCKED and exits 77 when ancestry is unknown or the helper belongs to Session 0.
`process_facts` reports boolean facts and API errors without account identifiers.
A skipped integration test is NOT VERIFIED, even when CTest exits successfully.
Run the following on a Windows desktop that satisfies these prerequisites:

```powershell
ctest --test-dir out/ci --output-on-failure -j 1 -R "^(process_facts|lock_policy|lock|lock_rename_e2e|lock_copy_e2e)$"
```

Mutation groups require a passing unmodified control. A crash, skip or timeout
blocks verdicts; each process deadline is separate from the whole group budget.
Gate mutants use an explicit CMake target rather than parsing Ninja shell text.

A completed control with exit code 77 propagates a reasoned CTest SKIP, never a
mutant kill. Supervisor errors, incomplete receipts and timeouts remain failures.
ConPTY image and lock scenarios first probe AppContainer profile creation and
Restart Manager directly through Windows APIs. Copy scenarios check that the
real destination ancestors are readable. Known unavailable OS prerequisites
produce SKIP with the error code retained; unexpected errors remain failures.
No alias or weaker product safety policy is substituted. Batch execution's full
control uses the ordinary test's existing budget for its recovery children and
large fixture; its fail-fast mutant hang limit stays unchanged.
Theme, hostile-image and fuzzy-worker controls likewise use their ordinary
test budgets: a healthy hosted MSVC Debug run can exceed the 15-second mutant
deadline. Session-place assertions run before the large wrapped-text fixture
so fail-fast session mutants reach their assertions promptly. Compilation
failures and control failures report the local evidence path; control failures
also report the supervisor state, exit code and elapsed time.

Mutation definitions must reach assertions without introducing incidental
undefined arithmetic, out-of-bounds scans or dangling pointers. Search-worker
tests synchronize in-flight enumeration with events and honor cancellation in
the fake enumerator, including a dedicated cancellation callback assertion.
Search fixtures use fresh owned areas with a nonce and manifest, and checked
native cleanup. A PID can be reused after a failed mutant retains its fixture;
reopening its hidden files with CREATE_ALWAYS can fail instead of testing search.

Sanitized links retain the entire selected compiler's ASan runtime thunk archive
and its SEH interceptor, matching clang-cl's link driver. Without this retention,
LLVM 23.1.3 can register DLL globals on load but omit their detach cleanup, so a
second load falsely reports the same literal as an ODR violation. ODR checking
stays enabled for both ordinary and mutation tests. `asan_pool_dll` repeatedly
loads and unloads an instrumented DLL; `sanitizer_detect_dll_global` verifies that
an actual global overrun is still detected after reloading it.

CMake 3.22 and newer prepend the selected runtime directory when each test runs,
instead of saving a developer shell's PATH during configuration. Mutation tools
restore that directory to the front after initializing VS, which may otherwise
select MSVC's different ASan DLL. Copied executable fixtures also carry their
runtime dependencies; isolated PATH and DLL search policies stay intact.

The mutation supervisor supplies redirected standard handles to a detached
process, avoiding an unnecessary `conhost.exe` whose asynchronous teardown can
outlive the test. It checks root exit and job accounting within the original run
deadline, using completion-port notifications to wake the accounting check.
A timeout, wait failure or surviving descendant still blocks a mutant verdict.

Run one CTest invocation at a time per session. Parallel execution inside that
invocation can use `-j 8`; reduce it to 4 or 2 when contention or load affects
results. DOS-drive users share `wf_dos_drive`. Mutation groups and their ordinary
drivers also share a driver lock, while groups with independent owned fixtures
can run together. Variable or nonzero DOS-drive requests keep the drive lock.
Elapsed-time checks for listing, editing and PDF decoding run without other
CTest work. Parallelism never changes their limits. Keep every failed attempt
when retrying, and reuse only completed individual results after interruption.

Handle-count tests initialize the requested Windows code-page cache before
their baseline: first non-ACP validation retains the Nls\\CodePage registry key
for the process lifetime. Worker handles still must return to the exact baseline.
Hash cancellation checks both absent completion and an idle worker; a hash still
running past the observation window must fail. Private-state fixtures explicitly
name TokenUser as owner, matching the product even with an elevated TokenOwner.
Lock E2E uses the ordinary lock test's process-facts prerequisite before asking
to close or terminate its owned helper. Unknown ancestry keeps those actions
refused and records SKIP. Failed E2E check identifiers are printed without their
free-text evidence so hosted logs identify the assertion while CSV stays local.

For a `product_tui` timeout, inspect the last `PRODUCT-TUI phase=...` line in
CTest's failure output. Each phase reports its start, result and elapsed time;
check results are flushed to the local CSV as they occur. These diagnostics do
not change the 150-second deadline or turn an interrupted scenario into a pass.

`keyboard_profiles` runs the same Vim/Nano profile checks on text fixtures
without image-decoder or Shell-association prerequisites. Editor entry waits
for its status separators, since the file manager also displays `NORMAL`.
The complete `product_tui` and `ui_audit` scenarios remain required.
