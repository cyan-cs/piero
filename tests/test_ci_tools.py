"""Synthetic CI repositories only; never initialize/change the source checkout's Git."""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import ci_changes
import ci_test_results
import check_secrets


class RepositoryTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out' / 'ci-tool-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.repo = Path(tempfile.mkdtemp(prefix='synthetic-', dir=parent)).resolve()
        self.assertTrue(self.repo.is_relative_to(parent.resolve()))
        self.previous = Path.cwd()
        self.environment = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        self.environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                                GIT_CONFIG_NOSYSTEM='1')
        self.env_patch = patch.dict(os.environ, self.environment, clear=True)
        self.env_patch.start()
        os.chdir(self.repo)
        self.addCleanup(self.restore)
        self.git('init', '--quiet', '--initial-branch=main')

    def restore(self):
        os.chdir(self.previous)
        self.env_patch.stop()
        # Retain owned synthetic fixtures under ignored out/ for inspection.

    def git(self, *args, data=None):
        return subprocess.run(['git', *args], input=data, capture_output=True, check=True).stdout

    def commit(self, files, parent=None, branch='main', merge=None):
        # fast-import creates a tiny NEW history from synthetic text. No git
        # commit, source-history transfer, hooks or persistent config is used.
        payload = f'commit refs/heads/{branch}\ncommitter CI fixture <ci@example.invalid> 1 +0000\ndata 7\nfixture\n'.encode()
        if parent:
            payload += f'from {parent}\n'.encode()
        if merge:
            payload += f'merge {merge}\n'.encode()
        payload += b'deleteall\n'
        for name, content in files.items():
            data = content.encode('utf-8')
            payload += f'M 100644 inline {json.dumps(name, ensure_ascii=False)}\ndata {len(data)}\n'.encode('utf-8') + data + b'\n'
        payload += b'\ndone\n'
        self.git('fast-import', '--quiet', data=payload)
        self.git('symbolic-ref', 'HEAD', f'refs/heads/{branch}')
        self.git('read-tree', '--reset', '-u', 'HEAD')
        return self.git('rev-parse', 'HEAD').decode().strip()

    def full(self, event='push', payload=None):
        return ci_changes.comparison(event, payload or {'before': '0' * 40})[:2]

    def scan(self, history=True):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            result = check_secrets.scan(history)
        return result, stream.getvalue()

    def test_first_commit_checks_clean_committed_whitespace(self):
        self.commit({'src/a.c': 'int value; \n'})
        self.assertEqual(self.git('status', '--porcelain'), b'')
        with self.assertRaisesRegex(RuntimeError, 'whitespace'):
            ci_changes.whitespace(*self.full())

    def test_first_commit_all_managed_files_with_spaces_and_unicode(self):
        self.commit({'src/space name.c': 'int a;\n', 'src/日本語.h': 'int b;\n',
                     'tools/a.h': 'int c;\n', 'tests/a.c': 'int d;\n',
                     'src/third_party/vendor.c': 'vendor\n', 'README.md': 'guide\n'})
        self.assertEqual(set(ci_changes.format_paths(*self.full())),
                         {'src/space name.c', 'src/日本語.h', 'tools/a.h', 'tests/a.c'})
        ci_changes.whitespace(*self.full())

    def test_crlf_is_valid_but_spaces_before_crlf_are_not(self):
        first = self.commit({'contract.txt': 'preserved\r\n'})
        ci_changes.whitespace(*self.full())
        self.commit({'contract.txt': 'preserved \r\n'}, first)
        with self.assertRaises(RuntimeError):
            ci_changes.whitespace(*self.full())

    def test_whitespace_excludes_vendor_but_checks_project_files(self):
        vendor = {'src/third_party/vendor.c': 'upstream \n\n',
                  'src/third_party/nested/data.uu': 'encoded \n'}
        first = self.commit({**vendor, 'src/a.c': 'int a;\n'})
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            ci_changes.whitespace(*self.full())
        self.assertIn('1 changed paths inspected', stream.getvalue())
        self.commit({**vendor, 'src/a.c': 'int a; \n'}, first)
        with self.assertRaisesRegex(RuntimeError, 'whitespace'):
            ci_changes.whitespace(*self.full(payload={'before': first}))

    def test_push_selects_changed_and_renamed_but_not_deleted_files(self):
        first = self.commit({'src/old.c': 'int a;\n', 'src/same.c': 'int b;\n', 'tests/gone.h': 'int c;\n'})
        self.commit({'src/new name.c': 'int a;\n', 'src/same.c': 'int b;\n'}, first)
        base, head = self.full(payload={'before': first})
        self.assertEqual(base, first)
        self.assertEqual(ci_changes.format_paths(base, head), ['src/new name.c'])

    def test_push_detects_committed_error(self):
        first = self.commit({'a.txt': 'clean\n'})
        self.commit({'a.txt': 'bad \n'}, first)
        with self.assertRaises(RuntimeError):
            ci_changes.whitespace(*self.full(payload={'before': first}))

    def test_new_branch_missing_and_malformed_base_check_full_tree(self):
        first = self.commit({'src/old.c': 'old \n'})
        self.commit({'src/old.c': 'old \n', 'src/new.c': 'int b;\n'}, first)
        for candidate in ('0' * 40, 'f' * 40, 'HEAD; echo untrusted'):
            with self.subTest(candidate=candidate):
                base, head = self.full(payload={'before': candidate})
                self.assertEqual(set(ci_changes.format_paths(base, head)), {'src/old.c', 'src/new.c'})
                with self.assertRaises(RuntimeError):
                    ci_changes.whitespace(base, head)

    def test_dispatch_checks_old_unchanged_errors(self):
        first = self.commit({'src/old.c': 'old \n'})
        self.commit({'src/old.c': 'old \n', 'a.txt': 'new\n'}, first)
        with self.assertRaises(RuntimeError):
            ci_changes.whitespace(*self.full('workflow_dispatch', {}))

    def test_known_empty_comparison_is_explicitly_completed(self):
        first = self.commit({'src/a.c': 'int a;\n'})
        base, head = self.full(payload={'before': first})
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            ci_changes.whitespace(base, head)
        self.assertIn('0 changed paths inspected', stream.getvalue())
        self.assertEqual(ci_changes.format_paths(base, head), [])

    def test_pr_merge_checkout_excludes_base_only_changes(self):
        root = self.commit({'src/root.c': 'int a;\n'})
        base = self.commit({'src/root.c': 'int a;\n', 'src/base.c': 'int b;\n'}, root)
        feature = self.commit({'src/root.c': 'int a;\n', 'src/feature.c': 'int c;\n'}, root, 'feature')
        self.commit({'src/root.c': 'int a;\n', 'src/base.c': 'int b;\n', 'src/feature.c': 'int c;\n'}, base, 'merge', feature)
        start, head = self.full('pull_request', {'pull_request': {'base': {'sha': base}}})
        self.assertEqual(start, base)
        self.assertEqual(ci_changes.format_paths(start, head), ['src/feature.c'])

    def test_pr_head_checkout_uses_merge_base(self):
        root = self.commit({'src/root.c': 'int a;\n'})
        base = self.commit({'src/root.c': 'int a;\n', 'src/base.c': 'int b;\n'}, root)
        self.commit({'src/root.c': 'int a;\n', 'src/feature.c': 'int c;\n'}, root, 'feature')
        start, head = self.full('pull_request', {'pull_request': {'base': {'sha': base}}})
        self.assertEqual(start, root)
        self.assertEqual(ci_changes.format_paths(start, head), ['src/feature.c'])

    def test_unrelated_pr_falls_back_to_full_tree(self):
        base = self.commit({'src/a.c': 'int a;\n'})
        self.commit({'src/other.c': 'bad \n'}, branch='other')
        with self.assertRaises(RuntimeError):
            ci_changes.whitespace(*self.full('pull_request', {'pull_request': {'base': {'sha': base}}}))

    def test_no_head_is_failure(self):
        with self.assertRaises(RuntimeError):
            self.full()

    def test_format_error_and_argument_boundaries(self):
        self.commit({'src/space name.c': 'int a;\n'})
        base, head = self.full()
        executable = self.repo / 'llvm/bin/clang-format.exe'
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b'not executed')
        with patch.dict(os.environ, PIERO_LLVM_ROOT=str(self.repo/'llvm')):
            with patch.object(ci_changes, 'format_paths', return_value=['src/space name.c']):
                with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 2)) as call:
                    with self.assertRaisesRegex(RuntimeError, 'clang-format'):
                        ci_changes.formatting(base, head)
                    self.assertEqual(call.call_args.args[0][-2:], ['--', 'src/space name.c'])

    def test_clean_initial_history_scan(self):
        self.commit({'a.txt': 'clean\n'})
        self.assertFalse(self.scan()[0])

    def test_shallow_history_is_not_pass(self):
        first = self.commit({'a.txt': 'clean\n'})
        (self.repo/'.git/shallow').write_text(first+'\n')
        with self.assertRaisesRegex(RuntimeError, 'complete checkout'):
            self.scan()

    def test_tracked_local_config_is_not_hidden_by_ignore(self):
        self.commit({'.gitignore': '.mcp.json\n', '.mcp.json': '{}\n'})
        self.assertTrue(self.scan()[0])
        (self.repo/'.mcp.json').unlink()
        self.assertTrue(self.scan()[0])

    def test_secret_in_deleted_history_never_prints_value(self):
        synthetic = 'gh' + 'p_' + 'A' * 36
        first = self.commit({'removed.txt': synthetic+'\n'})
        self.commit({'safe.txt': 'clean\n'}, first)
        bad, output = self.scan()
        self.assertTrue(bad)
        self.assertNotIn(synthetic, output)
        self.assertIn('historical', output)

    def test_current_untracked_content_also_scanned(self):
        self.commit({'safe.txt': 'clean\n'})
        synthetic = 'gh' + 'p_' + 'B' * 36
        Path('untracked.txt').write_text(synthetic)
        bad, output = self.scan(False)
        self.assertTrue(bad)
        self.assertNotIn(synthetic, output)


class ResultTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/ci-tool-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.folder = Path(tempfile.mkdtemp(prefix='results-', dir=parent))

    def report(self, xml):
        source = self.folder/'ctest.xml'
        source.write_text(xml, encoding='utf-8')
        return ci_test_results.summary(source)

    def test_only_verdicts_and_times_exported(self):
        result = self.report('<testsuite tests="3"><testcase name="good" time="0.2"><system-out>PRIVATE_SENTINEL</system-out></testcase><testcase name="bad"><failure message="PRIVATE_SENTINEL">PRIVATE_SENTINEL</failure></testcase><testcase name="skip"><skipped/></testcase></testsuite>')
        self.assertEqual(result['counts'], {'PASS':1, 'FAIL':1, 'SKIP':1, 'NOT RUN':0})
        self.assertNotIn('PRIVATE_SENTINEL', json.dumps(result))
        self.assertEqual(set(result['tests'][0]), {'name', 'status', 'seconds'})

    def test_empty_incomplete_unsafe_and_invalid_reports_fail(self):
        for xml in ('<testsuite/>', '<testsuite tests="2"><testcase name="only"/></testsuite>',
                    '<!DOCTYPE testsuite><testsuite/>', '<testsuite><testcase name="path/private"/></testsuite>',
                    '<testsuite><testcase name="nan" time="NaN"/></testsuite>',
                    '<testsuite><testcase name="unknown" status="mystery"/></testsuite>'):
            with self.subTest(xml=xml), self.assertRaises(ValueError):
                self.report(xml)

    def test_conpty_failure_exports_only_bounded_structured_diagnostics(self):
        line = ('CONPTY wait_failed reason=input_write elapsed_ms=12 '
                'input_bytes=4 output_bytes=512 parser=0 win32=109')
        result = self.report('<testsuite><testcase name="search_e2e">'
                             '<failure/><system-out>PRIVATE_SENTINEL\n' + line +
                             '\nFAIL check=search_empty_query_is_refused rep=0'
                             '\nNOT RUN check=dependent_checks reason=prerequisite_failed'
                             '\nFAIL check=PRIVATE_SENTINEL rep=0'
                             '\nCONPTY input_write_failed win32=109 sent=4'
                             '\nCONPTY wait_failed reason=PRIVATE_SENTINEL elapsed_ms=0'
                             '</system-out></testcase></testsuite>')
        diagnostics = result['tests'][0]['diagnostics']
        self.assertEqual(len(diagnostics), 4)
        self.assertEqual(diagnostics[0]['win32'], 109)
        self.assertEqual(diagnostics[2]['status'], 'NOT RUN')
        self.assertNotIn('PRIVATE_SENTINEL', json.dumps(result))
        long = self.report('<testsuite><testcase name="search_e2e"><failure/>'
                           '<system-out>' + '\n'.join([line] * 80) +
                           '</system-out></testcase></testsuite>')
        self.assertEqual(len(long['tests'][0]['diagnostics']), 32)

    def test_missing_report_records_not_run_and_fails(self):
        output=self.folder/'summary.json'
        with patch.object(sys, 'argv', ['ci_test_results', str(self.folder/'missing.xml'), str(output)]):
            self.assertNotEqual(ci_test_results.main(), 0)
        self.assertEqual(json.loads(output.read_text())['report_status'], 'NOT RUN')

    def test_contradictory_failure_totals_cannot_be_pass(self):
        with self.assertRaises(ValueError):
            self.report('<testsuite tests="1" failures="1"><testcase name="missing_failure"/></testsuite>')

    def test_utf16_entities_cannot_bypass_report_validation(self):
        source = self.folder / 'ctest.xml'
        xml = '<!DOCTYPE testsuite [<!ENTITY name "injected">]><testsuite tests="1"><testcase name="&name;"/></testsuite>'
        source.write_bytes(xml.encode('utf-16'))
        with self.assertRaises(ValueError):
            ci_test_results.summary(source)

    def test_report_read_is_bounded(self):
        limit = 16 * 1024 * 1024

        class BoundedStream(io.BytesIO):
            def read(self, size=-1):
                if not 0 <= size <= limit + 1:
                    raise AssertionError('Unbounded CI report read')
                return super().read(size)

        data = b'<testsuite tests="1"><testcase name="pass"/></testsuite>'
        with patch.object(Path, 'open', return_value=BoundedStream(data)):
            self.assertEqual(ci_test_results.summary(Path('fixture.xml'))['counts']['PASS'], 1)

    def test_local_only_names_and_public_fixtures(self):
        for name in ('.mcp.json', '.aws/credentials', '.env', '.env.production', 'out/a.exe', 'test.key', 'CMakeUserPresets.json'):
            self.assertTrue(check_secrets.local_only(name), name)
        for name in ('.env.example', '.mcp.example.json', 'tests/archive/safe.zip', 'src/third_party/lib/build/CMakeLists.txt'):
            self.assertFalse(check_secrets.local_only(name), name)


if __name__ == '__main__':
    unittest.main()
