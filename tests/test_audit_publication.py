"""Publication inventory failures use synthetic bytes and owned Git fixtures."""
import contextlib
import io
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import audit_publication
import test_ci_tools


class BatchProtocol(unittest.TestCase):
    oid = '1' * 40

    def run_batch(self, response, exit_code=0):
        process = MagicMock()
        process.__enter__.return_value = process
        process.stdin = io.BytesIO()
        process.stdout = io.BytesIO(response)
        process.wait.return_value = exit_code
        process.poll.return_value = exit_code
        found = []
        with patch.object(audit_publication.subprocess, 'Popen', return_value=process):
            count = audit_publication.historical_blobs(
                {self.oid: 'fixture.txt'}, lambda data, location: found.append((data, location)), [])
        return count, found

    def test_complete_response(self):
        count, found = self.run_batch(f'{self.oid} blob 4\n'.encode() + b'test\n')
        self.assertEqual(count, 1)
        self.assertEqual(found, [(b'test', dict(object=self.oid, path='fixture.txt'))])

    def test_empty_blobs_and_nonblob_objects(self):
        count, found = self.run_batch(f'{self.oid} blob 0\n\n'.encode())
        self.assertEqual(count, 1)
        self.assertEqual(found[0][0], b'')
        count, found = self.run_batch(f'{self.oid} tree 0\n\n'.encode())
        self.assertEqual((count, found), (0, []))

    def test_malformed_headers_and_payloads(self):
        for response in (b'', b'wrong missing\n', f'{self.oid} blob -1\n'.encode(),
                         f'{"2" * 40} blob 0\n\n'.encode(),
                         f'{self.oid} unknown 0\n\n'.encode(),
                         f'{self.oid} blob 5\nabc'.encode(),
                         f'{self.oid} blob 4\ntest!'.encode(),
                         f'{self.oid} blob 4\ntest\nextra'.encode()):
            with self.subTest(response=response):
                with self.assertRaises(RuntimeError):
                    self.run_batch(response)

    def test_nonzero_child_exit_cannot_qualify_a_complete_payload(self):
        with self.assertRaises(RuntimeError):
            self.run_batch(f'{self.oid} blob 4\ntest\n'.encode(), 1)

    def test_oversized_object_is_rejected_before_payload_read(self):
        with self.assertRaisesRegex(RuntimeError, 'read limit'):
            self.run_batch(f'{self.oid} blob {audit_publication.READ_LIMIT + 1}\n'.encode())

    def test_failure_terminates_a_child_still_writing(self):
        process = MagicMock()
        process.__enter__.return_value = process
        process.stdin = io.BytesIO()
        process.stdout = io.BytesIO(b'broken\n')
        process.poll.return_value = None
        with patch.object(audit_publication.subprocess, 'Popen', return_value=process):
            with self.assertRaises(RuntimeError):
                audit_publication.historical_blobs({self.oid: 'fixture'}, lambda *args: None, [])
        process.kill.assert_called_once()


class PublicationAudit(unittest.TestCase):
    def setUp(self):
        self.fixture = test_ci_tools.RepositoryTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.commit({'guide.txt': 'public fixture\n'})
        self.root = self.fixture.repo

    def test_real_git_history_and_unicode_worktree(self):
        name = '日本語 file.txt'
        (self.root / name).write_text('public@example.invalid\n', encoding='utf-8')
        before = (self.root / '.git/index').read_bytes()
        report = audit_publication.audit()
        self.assertEqual(report['audit_status'], 'COMPLETE')
        self.assertGreaterEqual(report['historical_blobs'], 1)
        self.assertTrue(any(item['location'] == dict(worktree=name) for item in report['findings']))
        self.assertEqual((self.root / '.git/index').read_bytes(), before)

    def test_main_writes_a_complete_report_for_real_git(self):
        output = self.root / 'audit.json'
        with patch.object(sys, 'argv', ['audit', '--output', str(output)]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(audit_publication.main(), 0)
        report = json.loads(output.read_text(encoding='utf-8'))
        self.assertEqual(report['audit_status'], 'COMPLETE')
        self.assertEqual(report['commits'], 1)
        self.assertGreater(report['historical_blobs'], 0)

    def test_subdirectory_run_still_inventories_the_whole_worktree(self):
        (self.root / 'notice.txt').write_bytes(b'outside@example.invalid\n')
        nested = self.root / 'nested'
        nested.mkdir()
        os.chdir(nested)
        try:
            report = audit_publication.audit()
        finally:
            os.chdir(self.root)
        self.assertTrue(any(item['location'] == dict(worktree='notice.txt') for item in report['findings']))

    def test_native_junction_refused_without_reading_target(self):
        if sys.platform != 'win32':
            self.skipTest('Windows junction fixture')
        (self.root / '.gitignore').write_text('owned-private/\n', encoding='utf-8')
        target = self.root / 'owned-private'
        target.mkdir()
        (target / 'data.txt').write_bytes(b'fixture target must not be read')
        link = self.root / 'link'
        made = subprocess.run(['cmd.exe', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True)
        if made.returncode:
            self.skipTest('Native junction creation unavailable')
        with patch.object(Path, 'open', side_effect=AssertionError('No worktree file may be opened')):
            with self.assertRaisesRegex(RuntimeError, 'reparse'):
                audit_publication.worktree_bytes(self.root, 'link/data.txt')
        with self.assertRaisesRegex(RuntimeError, 'reparse'):
            audit_publication.audit()

    def test_incomplete_run_writes_no_success_report_or_secret_diagnostic(self):
        output = self.root / 'report.json'
        stream = io.StringIO()
        with patch.object(sys, 'argv', ['audit', '--output', str(output)]), \
                patch.object(audit_publication, 'audit', side_effect=OSError('sensitive fixture bytes')), \
                contextlib.redirect_stdout(stream):
            self.assertEqual(audit_publication.main(), 1)
        self.assertFalse(output.exists())
        self.assertIn('INCOMPLETE', stream.getvalue())
        self.assertNotIn('sensitive fixture', stream.getvalue())

    def test_bounded_worktree_read_and_invalid_paths(self):
        for name in ('.', '../guide.txt', str(self.root / 'guide.txt')):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                audit_publication.worktree_bytes(self.root, name)
        with patch.object(audit_publication, 'READ_LIMIT', 4):
            with self.assertRaisesRegex(RuntimeError, 'read limit'):
                audit_publication.worktree_bytes(self.root, 'guide.txt')


if __name__ == '__main__':
    unittest.main()
