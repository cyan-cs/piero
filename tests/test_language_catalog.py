"""Validate INI sources/generation and adversarial translation documents."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shutil
import os

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from language_catalog import load, FILE_LIMIT, KEY_LIMIT
from generate_i18n import generate

class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.area = tempfile.TemporaryDirectory(prefix='language-parser-', dir=ROOT/'out')
        self.addCleanup(self.area.cleanup)
        self.root = Path(self.area.name)
        self.path = self.root/'fr.ini'
        self.base = load(ROOT/'languages/en.ini')
        self.reference = self.base['arguments']

    def parse(self, text):
        self.path.write_bytes(text if isinstance(text, bytes) else text.encode('utf-8'))
        return load(self.path, self.reference, self.base['messages'])

    def test_sources_and_generated_ids(self):
        en = load(ROOT/'languages/en.ini')
        ja = load(ROOT/'languages/ja.ini', en['arguments'])
        self.assertEqual(set(en['messages']), set(ja['messages']))
        generate(ROOT/'languages',self.root/'generated')
        ids=(self.root/'generated/wf_msg_ids.h').read_text(encoding='utf-8')
        for index,key in enumerate(en['messages']):
            self.assertIn(f'{key} = {index},',ids)

    def test_compiled_sources_have_no_retired_includes(self):
        names=['wf_'+'msgs.def','wf_'+'msgs_ja.def','wf_'+'langs.def']
        for name in names:
            self.assertFalse((ROOT/'src/i18n'/name).exists())
        paths=[ROOT/'CMakeLists.txt',*(ROOT/'cmake').glob('*.cmake')]
        paths += [p for p in (ROOT/'src').rglob('*')
                  if p.suffix in ('.c','.h') and 'third_party' not in p.parts]
        for path in paths:
            text=path.read_text(encoding='utf-8-sig')
            for name in names:
                self.assertNotIn(name,text,str(path.relative_to(ROOT)))

    def test_build_requires_both_complete_standard_files(self):
        source=self.root/'languages'
        source.mkdir()
        shutil.copyfile(ROOT/'languages/en.ini',source/'en.ini')
        with self.assertRaises(OSError): generate(source,self.root/'generated')
        (source/'ja.ini').write_text('[language]\ncode=ja\nname=日本語\n[messages]\nMSG_SETTINGS_TITLE=設定\n',encoding='utf-8')
        with self.assertRaises(ValueError): generate(source,self.root/'generated')

    def test_unicode_crlf_escapes_and_partial_override(self):
        d=self.parse('[language]\r\ncode=fr\r\nname=Français 日本語\r\n[messages]\r\nMSG_SETTINGS_TITLE=\\sCopier = oui; # littéral\\nDeux\\\\trois\\s\r\nfuture.key=ignored\r\n')
        self.assertEqual(d['messages']['MSG_SETTINGS_TITLE'],' Copier = oui; # littéral\nDeux\\trois ')

    def test_reject_unsafe_documents(self):
        prefix=b'[language]\ncode=fr\nname=F\n[messages]\nMSG_SETTINGS_TITLE='
        bad=[b'\x1b[31m',b'\0',b'\xc0\xaf',b'\xed\xa0\x80',b'\xc2\x9b',
             b'%n',b'%1$hn',b'{0}',b'}',b'\\q',b'x'*4097,
             b'A\nMSG_SETTINGS_TITLE=B',b'A\n[arguments]\nMSG_SETTINGS_TITLE=4',
             b'A\nunknown=X\nunknown=Y',
             b'A\nMSG_STATUS_RECOVERY_UNRESOLVED={0} {0}']
        for value in bad:
            with self.subTest(value_length=len(value)):
                with self.assertRaises(ValueError): self.parse(prefix+value+b'\n')
        for text in ('[messages]\nMSG_SETTINGS_TITLE=X',
                     '[language]\ncode=xx\nname=F\n[messages]\n',
                     '[language]\ncode=fr\nname=F\nplural=script\n[messages]\n'):
            with self.assertRaises(ValueError): self.parse(text)
        with self.assertRaises(ValueError): self.parse(b'x'*(FILE_LIMIT+1))
        with self.assertRaises(ValueError):
            self.parse('[language]\ncode=fr\nname=F\n[messages]\n'+''.join(f'x{i}=y\n' for i in range(KEY_LIMIT)))

def relocation(executable):
    with tempfile.TemporaryDirectory(prefix='language-relocation-',dir=ROOT/'out') as area:
        base=Path(area)
        target=base/'moved folder 日本語'
        target.mkdir()
        shutil.copyfile(executable,target/'language_probe.exe')
        shutil.copytree(ROOT/'languages',target/'languages')
        user=base/'local'/'piero'/'languages'
        user.mkdir(parents=True)
        for directory,value in [(target/'languages','Bundle relocation'),(user,'User relocation')]:
            (directory/'fr.ini').write_text('[language]\ncode=fr\nname=Français\n[messages]\nMSG_SETTINGS_TITLE='+value+'\n',encoding='utf-8')
        env=dict(os.environ,LOCALAPPDATA=str(base/'local'))
        result=subprocess.run([str(target/'language_probe.exe'),'--discover-executable'],
                              cwd=base,env=env,capture_output=True,timeout=30,
                              creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise RuntimeError('Relocated language discovery failed: '+str(result.returncode)+'\n'+result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
        print('PASS: relocated executable + separate CWD + isolated user-language priority')

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--relocate': relocation(Path(sys.argv[2]).resolve())
    else: unittest.main()
