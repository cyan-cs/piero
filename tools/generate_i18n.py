"""INI is authoritative. Emit stable IDs and emergency English data only."""
import argparse
import json
from pathlib import Path
from language_catalog import load_bundled

def literal(text):
    return json.dumps(text, ensure_ascii=False)

def generate(source, destination):
    en, ja = load_bundled(source)
    keys = list(en['messages'])
    if any(not k.startswith('MSG_') or not k.replace('_','').isalnum() for k in keys):
        raise ValueError('Message ID is not a C identifier')
    destination.mkdir(parents=True, exist_ok=True)
    ids = ['/* Generated from languages/en.ini. Do not edit. */',
           '#ifndef WF_MSG_IDS_H', '#define WF_MSG_IDS_H', 'typedef enum {']
    ids += [f'    {key} = {i},' for i,key in enumerate(keys)]
    ids += [f'    WF_MSG_COUNT = {len(keys)}', '} WfMsg;', '#endif', '']
    catalog = ['/* Generated from INI. Do not edit. */', 'static const Entry catalog[] = {']
    catalog += ['    {' + f'{literal(k)}, {en["arguments"][k]}, {literal(en["messages"][k])}' + '},' for k in keys]
    catalog += ['};', 'static const struct { const char *id, *name; bool none; } base_langs[] = {']
    catalog += ['    {' + f'{literal(d["code"])}, {literal(d["name"])}, {"true" if d["plural"]=="none" else "false"}' + '},' for d in (en,ja)]
    catalog += ['};', '']
    for name, lines in [('wf_msg_ids.h', ids),('wf_catalog_generated.h',catalog)]:
        data = '\n'.join(lines).encode('utf-8')
        path = destination / name
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source',type=Path)
    p.add_argument('destination',type=Path)
    a = p.parse_args()
    generate(a.source,a.destination)
