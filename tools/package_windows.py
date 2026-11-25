"""Prepare local candidate ZIP/installer payload. Never publishes or installs."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import zipfile
from pathlib import Path
from pe_imports import inventory
from build_version import valid as valid_build_version
from language_catalog import load as load_language, load_bundled, FILE_LIMIT

ROOT=Path(__file__).resolve().parents[1]
SYSTEM={'advapi32.dll','bcrypt.dll','combase.dll','comdlg32.dll','gdi32.dll','imm32.dll',
        'kernel32.dll','ntdll.dll','ole32.dll','oleaut32.dll','shell32.dll','shcore.dll',
        'shlwapi.dll','user32.dll','userenv.dll','winhttp.dll','ws2_32.dll','runtimeobject.dll',
        'rpcrt4.dll','rstrtmgr.dll','secur32.dll','version.dll','normaliz.dll','ucrtbase.dll','msvcrt.dll'}


def prepare(build,output):
    build=build.resolve(strict=True)
    output=output.resolve()
    if not output.is_relative_to(ROOT/'out') or output==ROOT/'out' or output.exists():
        raise ValueError('Choose a NEW output directory below workspace out/')
    cache=(build/'CMakeCache.txt').read_text(encoding='utf-8')
    def setting(key):
        match=re.search(r'^'+key+r':[^=]+=(.*)$',cache,re.M)
        return match[1] if match else ''
    if setting('CMAKE_BUILD_TYPE')!='Release' or setting('WF_SANITIZE')!='OFF' or setting('CMAKE_MSVC_RUNTIME_LIBRARY')!='MultiThreaded':
        raise ValueError('Requires unsanitized Release with the static CRT (MultiThreaded)')
    version=setting('PIERO_VERSION')
    if not valid_build_version(version):
        raise ValueError('Invalid package version')
    files={'piero.exe':build/'piero.exe','piero-path.exe':build/'piero_path.exe','LICENSE':ROOT/'LICENSE'}
    files.update({'LICENSES/'+p.name:p for p in (ROOT/'LICENSES').glob('*.txt')})
    base_language=load_bundled(ROOT/'languages')[0]
    language_hashes={}
    for language in sorted((ROOT/'languages').glob('*.ini')):
        validated=load_language(language,base_language['arguments'],base_language['messages'])
        language_hashes['languages/'+language.name]=validated['source_sha256']
        files['languages/'+language.name]=language
    pe={}
    for name in ('piero.exe','piero-path.exe'):
        info=inventory(files[name]); pe[name]=info
        if any(d not in SYSTEM and not d.startswith(('api-ms-win-','ext-ms-win-')) for d in info['imports']):
            raise ValueError('Unreviewed/non-system DLL dependency; do not copy arbitrary runtimes')
    measured=subprocess.run([str(files['piero.exe']),'--version'],capture_output=True,check=True,timeout=15).stdout.decode().strip()
    if measured!='piero '+version: raise ValueError('Executable/cache version mismatch')
    output.mkdir(parents=True)
    stage=output/'payload'; stage.mkdir()
    manifest=[]
    for name,source in sorted(files.items()):
        if source.is_symlink(): raise ValueError('Unexpected source link')
        destination=stage/name; destination.parent.mkdir(parents=True,exist_ok=True)
        if name in language_hashes:
            with source.open('rb') as stream: data=stream.read(FILE_LIMIT+1)
            if len(data)>FILE_LIMIT or hashlib.sha256(data).hexdigest()!=language_hashes[name]:
                raise ValueError('Language changed after validation; preserve this failed stage')
            destination.write_bytes(data)
        else:
            shutil.copyfile(source,destination)
            data=source.read_bytes()
        if destination.read_bytes()!=data: raise ValueError('Copy mismatch')
        manifest.append({'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    with zipfile.ZipFile(output/'piero-windows-x64.zip','x',compression=zipfile.ZIP_DEFLATED) as archive:
        for item in manifest: archive.write(stage/item['path'],item['path'])
    with zipfile.ZipFile(output/'piero-windows-x64.zip') as archive:
        if set(archive.namelist())!={m['path'] for m in manifest}: raise ValueError('ZIP inventory mismatch')
        for item in manifest:
            if hashlib.sha256(archive.read(item['path'])).hexdigest()!=item['sha256']: raise ValueError('ZIP hash mismatch')
    report={'version':version,'payload':manifest,'pe':pe,'installer':'NOT BUILT; compile packaging/piero.iss with this payload',
            'install_uninstall_PATH':'NOT RUN; disposable VM required','release_status':'RELEASE BLOCKED'}
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Prepared candidate portable ZIP and matching installer payload: '+str(output))
    print('Not an approved release. Installation, PATH changes and signature trust are not qualified.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    prepare(args.build,args.output)
