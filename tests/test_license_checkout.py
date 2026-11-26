"""The legal text remains byte-identical across Windows checkout preferences."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LicenseCheckout(unittest.TestCase):
    def test_fresh_checkout_preserves_license_bytes(self):
        base = ROOT / 'out/license-checkout-tests'
        base.mkdir(parents=True, exist_ok=True)
        license_bytes = (ROOT / 'LICENSE').read_bytes()
        self.assertNotIn(b'\r', license_bytes)
        environment = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                           GIT_CONFIG_NOSYSTEM='1')
        for autocrlf in ('true', 'false'):
            with self.subTest(autocrlf=autocrlf), tempfile.TemporaryDirectory(dir=base) as directory:
                def git(*args, data=None):
                    return subprocess.run(['git', '-C', directory, *args], input=data,
                                          capture_output=True, check=True, env=environment).stdout
                git('init', '-q')
                payload = b'commit refs/heads/main\ncommitter Fixture <ci@example.invalid> 1 +0000\ndata 7\nfixture\n'
                for name, data in (('.gitattributes', (ROOT / '.gitattributes').read_bytes()),
                                   ('LICENSE', license_bytes)):
                    payload += f'M 100644 inline {json.dumps(name)}\ndata {len(data)}\n'.encode() + data + b'\n'
                git('fast-import', '--quiet', data=payload + b'\ndone\n')
                git('-c', 'core.autocrlf=' + autocrlf, 'checkout', '-f', 'main')
                checkout = (Path(directory) / 'LICENSE').read_bytes()
                self.assertEqual(checkout, git('show', 'HEAD:LICENSE'))
                self.assertEqual(checkout, license_bytes)


if __name__ == '__main__':
    unittest.main()
