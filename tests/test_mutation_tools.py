"""Mutation verdicts require completed assertions, and anchors preserve literals."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'Windows PowerShell mutation harness')
class MutationTools(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/mutation-tool-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def run_script(self, body):
        script = self.root / 'driver.ps1'
        script.write_text("""param([string]$Helper,[string]$Directory)
$ErrorActionPreference='Stop'
. $Helper
""" + body, encoding='utf-8')
        result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(script), '-Helper', str(ROOT / 'tools/p0/wf_mutation_text.ps1'),
                                 '-Directory', str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout.split('RESULT_JSON:')[-1])

    def test_crashes_skips_and_other_failures_are_never_kills(self):
        (self.root / 'test.log').write_text('FAIL fixture.c:1: assertion\n', encoding='utf-8')
        results = self.run_script("""
$results=@()
foreach($code in @(0,1,2,77,-1073741819)) {
  $arguments=@{Name='fixture';Directory=$Directory;ExitCode=$code}
  if((Get-Command Test-WfMutant).Parameters.ContainsKey('AnyFailure')) { $arguments.AnyFailure=$true }
  $results += Test-WfMutant @arguments
}
'RESULT_JSON:'+ (ConvertTo-Json -Compress -InputObject $results)
""")
        self.assertEqual([item['result'] == 'KILLED' for item in results],
                         [False, True, False, False, False])

    def test_quoted_fail_text_is_not_an_assertion(self):
        for text in ('Captured text: FAIL fixture\n', 'fail to open fixture\n'):
            (self.root / 'test.log').write_text(text, encoding='utf-8')
            result = self.run_script("""
$result=Test-WfMutant 'fixture' $Directory 1
'RESULT_JSON:'+ (ConvertTo-Json -Compress -InputObject $result)
""")
            self.assertNotEqual(result['result'], 'KILLED')

    def test_empty_group_does_not_pass(self):
        result = self.run_script("""
try { $code=Complete-WfMutations @() $Directory $null; $passed=$code -eq 0 }
catch { $passed=$false }
'RESULT_JSON:'+ (ConvertTo-Json -Compress -InputObject @{passed=$passed})
""")
        self.assertFalse(result['passed'])

    def test_layout_and_quoted_literal_boundaries(self):
        result = self.run_script("""
$cases=@(
  @{source='return   value ;';anchor='return value;';count=1},
  @{source='returnvalue;';anchor='return value;';count=0},
  @{source='return values;';anchor='return value;';count=0},
  @{source='f("a  b");';anchor='f("a b");';count=0},
  @{source='f ( "a b" ) ;';anchor='f("a b");';count=1},
  @{source='f("a\\\"  b");';anchor='f("a\\\" b");';count=0}
)
$results=@($cases | ForEach-Object { @{actual=(Measure-WfAnchor $_.source $_.anchor);expected=$_.count} })
'RESULT_JSON:'+ (ConvertTo-Json -Compress -InputObject $results)
""")
        self.assertEqual([item['actual'] for item in result], [item['expected'] for item in result])


if __name__ == '__main__':
    unittest.main()
