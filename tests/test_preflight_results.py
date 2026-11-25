"""Check CTest evidence matching without launching product tests or builds."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'PowerShell evidence validator')
class PreflightResults(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out' / 'preflight-result-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=parent))
        self.runner = self.directory / 'run.ps1'
        self.runner.write_text("""param([string]$Helper,[string]$Manifest,[string]$Report)
$ErrorActionPreference='Stop'
. $Helper
Read-WfCtestPassed $Manifest $Report
""", encoding='utf-8')

    def run_case(self, registered, cases=None, *, attributes=None, change=None, raw=None):
        manifest = self.directory / 'manifest.json'
        manifest.write_text(json.dumps(dict(kind='ctestInfo', version=dict(major=1, minor=0),
                                           tests=[dict(name=name) for name in registered])), encoding='utf-8-sig')
        report = self.directory / 'report.xml'
        suite = ET.Element('testsuite', name='Win32-ninja', tests=str(len(registered)), failures='0', skipped='0')
        suite.attrib.update(attributes or {})
        for name in registered if cases is None else cases:
            ET.SubElement(suite, 'testcase', name=name, status='run')
        if change:
            change(suite)
        report.write_bytes(raw if raw is not None else ET.tostring(suite, encoding='utf-8'))
        powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        return subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                               '-File', str(self.runner), '-Helper', str(ROOT / 'tools/p0/wf_ctest_results.ps1'),
                               '-Manifest', str(manifest), '-Report', str(report)], capture_output=True, timeout=20)

    def test_current_manifest_controls_total_and_ordinal_names(self):
        for count in (2, 27, 374):
            with self.subTest(count=count):
                names = ['Alpha', 'alpha'] + [f'test_{i}' for i in range(count - 2)]
                result = self.run_case(names, list(reversed(names)))
                self.assertEqual(result.returncode, 0, repr(result.stderr))
                self.assertEqual(int(result.stdout), count)

    def test_missing_duplicate_and_unexpected_results_are_rejected(self):
        for registered, cases in (([], []), (['a', 'a'], ['a', 'a']),
                                  (['a', 'b'], ['a']), (['a', 'b'], ['a', 'a']),
                                  (['a', 'b'], ['a', 'c'])):
            with self.subTest(registered=registered, cases=cases):
                self.assertNotEqual(self.run_case(registered, cases).returncode, 0)

    def test_failures_skips_errors_and_disabled_cases_are_rejected(self):
        for attribute in ('failures', 'errors', 'skipped', 'disabled', 'tests'):
            with self.subTest(attribute=attribute):
                self.assertNotEqual(self.run_case(['a'], attributes={attribute: '2'}).returncode, 0)
        for child in ('failure', 'error', 'skipped'):
            with self.subTest(child=child):
                self.assertNotEqual(self.run_case(['a'], change=lambda s: ET.SubElement(s[0], child)).returncode, 0)
        self.assertNotEqual(self.run_case(['a'], change=lambda s: s[0].set('status', 'notrun')).returncode, 0)

    def test_dtd_entities_are_rejected(self):
        xml = b'<!DOCTYPE testsuite [<!ENTITY name "a">]><testsuite tests="1"><testcase name="&name;" status="run"/></testsuite>'
        self.assertNotEqual(self.run_case(['a'], raw=xml).returncode, 0)


if __name__ == '__main__':
    unittest.main()
