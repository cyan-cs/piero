"""Publish only an allowlisted CTest result summary, never raw test output."""
import argparse
import json
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path


def summary(path):
    limit = 16 * 1024 * 1024
    with path.open('rb') as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError('Unsafe or oversized report')
    text = raw.decode('utf-8-sig')
    if '\0' in text or '<!DOCTYPE' in text or '<!ENTITY' in text:
        raise ValueError('Unsafe report declaration')
    root = ET.fromstring(text)
    if root.tag not in ('testsuite', 'testsuites'):
        raise ValueError('Not a JUnit report')
    tests = []
    elements = {'failures': 0, 'errors': 0, 'skipped': 0}
    for case in root.iter('testcase'):
        name = case.get('name', '')
        if not re.fullmatch(r'[A-Za-z0-9_.:-]{1,128}', name):
            raise ValueError('Unexpected test identifier')
        seconds = float(case.get('time', '0'))
        if not math.isfinite(seconds) or seconds < 0:
            raise ValueError('Invalid duration')
        for attribute, child in (('failures', 'failure'), ('errors', 'error'), ('skipped', 'skipped')):
            elements[attribute] += int(case.find(child) is not None)
        if case.find('failure') is not None or case.find('error') is not None:
            status = 'FAIL'
        elif case.find('skipped') is not None:
            status = 'SKIP'
        elif case.get('status') == 'notrun':
            status = 'NOT RUN'
        elif case.get('status', 'run') in ('run', ''):
            status = 'PASS'
        else:
            raise ValueError('Unknown test status')
        tests.append(dict(name=name, status=status, seconds=seconds))
    if not tests or len({x['name'] for x in tests}) != len(tests):
        raise ValueError('Empty or ambiguous report')
    if 'tests' in root.attrib and int(root.attrib['tests']) != len(tests):
        raise ValueError('Incomplete report')
    if any(key in root.attrib and int(root.attrib[key]) != value for key, value in elements.items()):
        raise ValueError('Contradictory report totals')
    counts = {s: sum(t['status'] == s for t in tests) for s in ('PASS', 'FAIL', 'SKIP', 'NOT RUN')}
    return dict(schema=1, report_status='COMPLETE', counts=counts, tests=tests,
                release_status='RELEASE BLOCKED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('junit', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        result = summary(args.junit)
    except FileNotFoundError:
        result = dict(schema=1, report_status='NOT RUN', reason='CTest report absent; configure/build/test may not have completed', release_status='RELEASE BLOCKED')
    except (OSError, ValueError, ET.ParseError):
        result = dict(schema=1, report_status='INVALID', reason='CTest report could not be validated', release_status='RELEASE BLOCKED')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('CTest artifact summary: ' + result['report_status'] + ' (raw output excluded)')
    return int(result['report_status'] != 'COMPLETE' or result.get('counts', {}).get('FAIL', 0) or result.get('counts', {}).get('NOT RUN', 0))


if __name__ == '__main__':
    raise SystemExit(bool(main()))
