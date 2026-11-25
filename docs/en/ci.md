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

Sources: [checkout release](https://github.com/actions/checkout/releases/tag/v4.3.1),
[Python release](https://github.com/actions/setup-python/releases/tag/v5.6.0),
[artifact release](https://github.com/actions/upload-artifact/releases/tag/v4.6.2).
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
CRLF is accepted as a line ending; spaces before it still fail. Pinned contract
bytes are not normalized by this check. Retained evidence/vendor files may have
historical whitespace errors. Review failures without disabling the check or
silently rewriting pinned evidence to make it pass.

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

Push validation stays enabled for development branches as well as the default
branch. Automatic runs supersede the same event/ref; PR refs stay separate.
Manual dispatch uses a unique run identifier, so an automatic push or another
manual analysis run does not cancel it through the concurrency group.

Both compilers retain Debug/Release builds and full CTest; Debug ASan/UBSan,
formatting, documentation, secrets and optional MSVC analysis remain present.
No known failure becomes a warning, skip or ignored exit. Hosted-runner and
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
