"""Freeze explicitly scoped changes without writing the Git index or history."""
import argparse
import hashlib
import json
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=Path('out/publication-audit/final-handoff'))
parser.add_argument('--message', default='fix: preserve data and prepare bilingual development preview')
args = parser.parse_args()
OUT = (ROOT / args.output).resolve()
if not OUT.is_relative_to((ROOT / 'out').resolve()):
    raise SystemExit('Handoff output must stay inside the workspace out directory')
if OUT.exists():
    raise SystemExit('Refusing an existing handoff directory; archive/review it explicitly first')
OUT.mkdir(parents=True)


def git(*args):
    return subprocess.check_output(['git', '--literal-pathspecs', *args], cwd=ROOT)


def git_paths(*args):
    return {path for path in git(*args).decode('utf-8').split('\0') if path}


paths = git_paths('diff', '--name-only', '-z', 'HEAD')
new = git_paths('ls-files', '-z', '--others', '--exclude-standard')
allowed = ('src/', 'tests/', 'tools/', 'cmake/', 'docs/', 'assets/', '.github/')
root_names = {'LICENSE', 'CHANGELOG.md', 'SECURITY.md', 'CODING_STYLE.md', '.mcp.example.json'}
paths.update(p for p in new if p.startswith(allowed) or p in root_names)
paths = sorted(p for p in paths if p.lower() != '.mcp.json' and
               not p.lower().startswith(('.superpowers/', '.codex/', '.agents/')))
tracked = git_paths('ls-files', '-z')
for path in paths:
    current = ROOT
    for part in Path(path).parts:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            break
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise SystemExit('Reparse point in handoff path: ' + path)
patch = git('diff', '--binary', 'HEAD', '--', *[p for p in paths if p in tracked])
for path in paths:
    if path in tracked:
        continue
    result = subprocess.run(['git', '--literal-pathspecs', 'diff', '--no-index', '--binary', '--', '/dev/null', path], cwd=ROOT, capture_output=True)
    if result.returncode != 1:
        raise SystemExit('Cannot generate new-file patch for ' + path)
    patch += result.stdout
patch_path = OUT / 'publication.patch'
patch_path.write_bytes(patch)
check = subprocess.run(['git', 'apply', '--reverse', '--check', '--', str(patch_path)], cwd=ROOT, capture_output=True)
if check.returncode:
    raise SystemExit('Reverse check failed; preserve handoff and investigate: ' + check.stderr.decode(errors='replace'))
keep_local = [p for p in tracked if p.lower() == '.mcp.json' or p.lower().startswith('.superpowers/')]
manifest = dict(base=git('rev-parse', 'HEAD').decode().strip(),
                patch='publication.patch', patch_sha256=hashlib.sha256(patch).hexdigest(),
                files=[dict(path=p, sha256=hashlib.sha256((ROOT/p).read_bytes()).hexdigest()
                            if (ROOT/p).is_file() else None) for p in paths],
                untrack_keep_local=sorted(keep_local),
                message=args.message,
                note='Already applied to this worktree. Do not apply again. No history privacy cleanup is implied.')
(OUT/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8', newline='\n')
print(f'Frozen {len(paths)} files; reverse-apply check PASS; no Git writes')
