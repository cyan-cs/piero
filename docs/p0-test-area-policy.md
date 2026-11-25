# P0 scratch-area policy (harness rule, not a spec change)

* P0 tools and Windows integration tests that use `P0Area` create one directory per run:
  `<base>\wfile-p0-<run_id>\` containing `.p0-owner` (nonce + root FileId) and
  `.p0-manifest` (everything the run created).
* Removal is only ever of a **recorded** object, by exact path, after
  re-proving: root FileId, marker nonce, exact recorded path, parent chain
  resolving inside the root (final-path compare; a junction is refused).
  Directories are removed with `RemoveDirectoryW` (empty only).
* Forbidden: wildcard or recursive removal without ownership proof, and deleting
  merely because a name matches a pattern in `%TEMP%` or another shared location.
  A pattern may discover candidate areas; it never authorizes removal. The
  marker, identity, parent chain and per-object checks still apply.
* Leftovers keep the marker/manifest so `p0_area_reap <dir>` can finish later;
  a directory without a valid marker is refused.
* Implementation: `tools/p0/p0_area.c`; tests: `tests/test_p0_area.c`.

## Areas left by killed runs (large fixtures)

A run that is killed (a CTest timeout, a crash) cannot clean up after itself.
Large fixtures (the transfer scenarios write files of hundreds of MiB) then
pile up and can fill the system volume, which also shrinks the page file's
room (the commit limit). Three measures:

* **Before writing, check the disk.** `p0_free_space_for(dir, bytes)`
  (`tools/p0/p0_common.c`) is true only if the volume has the fixtures' size
  plus a 4 GiB reserve free. A test that writes large files calls it first and
  stops without writing a byte when it is false.
* **Attempt cleanup on every ordinary exit.** Transfer scenarios try to remove
  their large fixtures after success, failure or cancellation. A forced termination
  or cleanup failure can leave files; keep their marker and manifest.
* **Sweep what a killed run left.** `p0_scratch_sweep <base> [--reap]`
  (`tools/p0/p0_scratch_sweep.c`, tested by `tests/test_scratch_sweep.c`).
  Without `--reap` it only lists. A directory is touched only if its name has
  the shape `wfile-p0-<label>-<pid>-<id>`, **no process with that pid is
  running**, its `.p0-owner` marker and root FileId verify, and it is a real
  directory (not a link). `--reap` first removes the known large files by
  exact place (`big.bin` and the copy engine's `.wfcopy-*.tmp` in the fixture
  folders of the transfer scenarios; only plain files with one name, proven
  inside the area), then `p0_area_destroy` removes what the run recorded.
  Unexpected content is named and kept, so a directory can remain. A live
  run's area, a directory without a valid marker, and a hard-linked file are
  never touched.

Run it by hand on a build tree's `preflight-scratch` folder, never on a shared
location. It is not run automatically.
