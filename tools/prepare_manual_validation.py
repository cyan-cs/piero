"""Prepare synthetic visual-test inputs, never run a terminal or destructive trial."""
import argparse
import csv
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import uuid
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OWNER = '.piero-manual-owner.json'


def scoped(path):
    path = path.resolve()
    if not path.is_relative_to((REPO/'out').resolve()) or path == (REPO/'out').resolve():
        raise SystemExit('Manual fixtures must stay in a child of the workspace out directory')
    return path


def verify(path):
    path = scoped(path)
    owner = json.loads((path/OWNER).read_text(encoding='utf-8'))
    info = path.stat()
    if owner['root'] != str(path) or owner['file_id'] != info.st_ino or owner['device'] != info.st_dev:
        raise SystemExit('Manual fixture identity changed; preserve it and stop')
    print('Manual root identity matches; this does not prove VM ownership or test success')


def prepare(output, executable):
    output = scoped(output)
    executable = executable.resolve(strict=True)
    if not executable.is_relative_to(REPO/'out'):
        raise SystemExit('Select a reviewed workspace build under out/')
    generator = executable.parent/'wic_proto.exe'
    if not generator.is_file():
        raise SystemExit('Build the wic_proto target beside the selected product first')
    if output.exists():
        raise SystemExit('Refusing an existing fixture directory')
    output.mkdir(parents=True)
    info = output.stat()
    if not info.st_ino:
        raise SystemExit('Filesystem did not provide a root identity; stop')
    owner = dict(root=str(output), file_id=info.st_ino, device=info.st_dev,
                 run_id=str(uuid.uuid4()), identity_source='os.stat on Windows',
                 purpose='Synthetic manual visual inputs; no cleanup or recovery authority')
    (output/OWNER).write_text(json.dumps(owner, indent=2)+'\n', encoding='utf-8')
    data = output/'data'
    images = data/'images'
    images.mkdir(parents=True)
    result = subprocess.run([str(generator), 'gen', str(images)], capture_output=True, timeout=60)
    (output/'image-generator.log').write_bytes(result.stdout + result.stderr)
    if result.returncode or not (images/'grad.png').is_file() or not (images/'grad.jpg').is_file():
        raise SystemExit('Synthetic image generation failed; retain the incomplete fixture')
    if not (images/'grad.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n') or not (images/'grad.jpg').read_bytes().startswith(b'\xff\xd8\xff'):
        raise SystemExit('Expected PNG/JPEG signatures missing')
    text = 'piero manual sample\n日本語: ひらがな・カタカナ・漢字\n'
    (data/'utf8.txt').write_text(text, encoding='utf-8', newline='\n')
    (data/'utf16.txt').write_text(text, encoding='utf-16')
    (data/'日本語.txt').write_text(text, encoding='utf-8', newline='\n')
    (data/'large.txt').write_text('line: 0123456789 abcdefghijklmnopqrstuvwxyz\n' * 1600, encoding='utf-8')
    (data/'empty-folder').mkdir()
    (data/'日本語フォルダ').mkdir()
    subprocess.run([sys.executable, str(REPO/'tools/proto/make_pdfs.py'), str(data/'pdf')], check=True)
    with zipfile.ZipFile(data/'sample.zip', 'w') as archive:
        archive.writestr('日本語/説明.txt', text.encode('utf-8'))
        archive.writestr('images/grad.png', (images/'grad.png').read_bytes())
    with tarfile.open(data/'sample.tar', 'w') as archive:
        raw = text.encode('utf-8')
        member = tarfile.TarInfo('日本語/説明.txt')
        member.size = len(raw)
        archive.addfile(member, io.BytesIO(raw))
    configs = output/'config'
    configs.mkdir()
    for profile in ('standard', 'nano', 'vim', 'custom'):
        for graphics in ('auto', 'sixel', 'text'):
            body = ('[meta]\nschema_version=1\n[general]\nlanguage=ja\n'
                    f'[keyboard]\nprofile={profile}\nbindings_version=2\n'
                    '[keyboard.custom]\nbase=nano\n'
                    f'[preview]\nimages=true\ngraphics={graphics}\n'
                    '[launch]\nworking_directory=neutral\n')
            (configs/f'{profile}-{graphics}.ini').write_text(body, encoding='utf-8', newline='\n')
    for name in ('state/roaming', 'state/local', 'evidence'):
        (output/name).mkdir(parents=True)
    rows = []
    areas = {
        'WT': 'start exit columns fast-navigation folders prefetch navigation-memory filter sort status settings',
        'PV': 'utf8 utf16 japanese large-text png jpeg pdf encrypted-pdf zip tar fullscreen',
        'SX': 'auto explicit fallback image-switch zoom resize font dialog clear ghosting restore',
        'IN': 'ime jis alphanumeric symbols chords nano vim custom editor-japanese',
        'UX': 'idle-30s unrelated-change same-content content-change replacement sixel-bytes resize loading notice',
        'FS': 'create rename copy same-volume-move recycle undo-rename undo-copy cancel pause crash-recovery editor-save backup ads file-id separate-volume-copy cross-volume-move overwrite-copy',
        'POWER': 'normal-exit process-crash forced-termination os-crash vm-hard-off S14 S15',
    }
    expectations = {
        'WT': 'Requested navigation/state is correct; no lost input or spurious preview clear; terminal restored on exit.',
        'PV': 'Correct selected content/encoding; bounded safe error on failure; no unisolated decoding or cross-file stale content.',
        'SX': 'Only verified capabilities used; text fallback works; image placement/erase/restoration correct with no ghosting.',
        'IN': 'Active keymap and Japanese/English input match documented actions; composition/cancellation loses no text.',
        'UX': 'Unchanged selection/content causes no unwanted loading/clear/redecode; real changes and replacement update correctly.',
        'FS': 'VM/disk/snapshot preflight recorded; originals/backups/identities/ADS match expected results; errors never count as success.',
        'POWER': 'VM-only preflight and cut phase recorded; reboot hashes and retained originals/backups/journals match the pinned contract.',
    }
    special = {
        'recycle': 'Cancel leaves the source unchanged; confirmation recycles only the selected items. Inspect per-item results and the Recycle Bin.',
        'undo-copy': 'After confirmation, Undo recycles only the new copies whose identities still match; originals remain unchanged.',
        'cross-volume-move': 'Verify destination data before recycling each source; failed or cancelled unfinished items retain their originals. Inspect partial results.',
        'overwrite-copy': 'Cancel leaves the existing destination unchanged; confirmed overwrite replaces its contents and permanently removes the old file. No Undo.',
        'idle-30s': 'No unwanted preview generation/clear/loading or Sixel resend during unchanged selection.',
        'S14': '300 actual VM hard-off/reboot/inspection trials across at least 3 size classes; count only executed cuts.',
        'S15': 'Committed payload retained; only owned empty metadata retired; unexpected children/identity mismatch require manual review.',
    }
    for area, scenarios in areas.items():
        for scenario in scenarios.split():
            rows.append(dict(case_id=f'{area}-{scenario}', area=area, result='NOT RUN',
                             expected=special.get(scenario, expectations[area]), observed='',
                             observer='', conditions='', evidence=''))
    with (output/'results.csv').open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys(), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)
    metadata = dict(repository=str(REPO), product=str(executable), product_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                    preparation='COMPLETE', manual_cases='NOT RUN', vm_ownership='NOT VERIFIED',
                    snapshot_reference='', dedicated_volume_identity='NOT VERIFIED',
                    measured_power_loss_iterations=0, encrypted_pdf_test_password='secret',
                    instruction='No destructive file-operation test is authorized by this marker. Verify the disposable VM/disks/snapshot separately.')
    (output/'environment.json').write_text(json.dumps(metadata, indent=2)+'\n', encoding='utf-8')
    launcher = (REPO/'tools/manual_terminal_session.ps1').read_text(encoding='utf-8')
    (output/'run-terminal.ps1').write_text(launcher, encoding='utf-8', newline='\n')
    inventory = [dict(path=p.relative_to(output).as_posix(), bytes=p.stat().st_size,
                      sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in data.rglob('*') if p.is_file()]
    (output/'samples.json').write_text(json.dumps(inventory, indent=2)+'\n', encoding='utf-8')
    print(f'Prepared {len(inventory)} synthetic sample files and {len(rows)} NOT RUN cases under {output}')
    print('No terminal was launched, no destructive trial ran, and no VM ownership was inferred')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This prepares native Windows validation only')
    if args.verify:
        if args.output or args.executable:
            parser.error('--verify cannot prepare new files')
        verify(args.verify)
    elif args.output and args.executable:
        prepare(args.output, args.executable)
    else:
        parser.error('Use --output <new-directory> --executable <reviewed-build> or --verify <directory>')
