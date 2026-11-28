"""Publish only an allowlisted CTest result summary, never raw test output."""
import argparse
import json
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path


CONPTY_CHECKS = frozenset({
    'dependent_checks',
    'dirsize_changes_no_file',
    'dirsize_r_forgets_the_totals',
    'dirsize_rows_start_without_sizes',
    'dirsize_state_line_says_how_many',
    'dirsize_totals_are_drawn',
    'dupes_cancel_changes_nothing',
    'dupes_copy_is_not_offered',
    'dupes_ctrl_a_is_not_offered',
    'dupes_delete_asks_once',
    'dupes_finds_the_three_equal_files',
    'dupes_list_is_shown',
    'dupes_lists_the_group_and_nothing_else',
    'dupes_the_last_copy_cannot_be_marked',
    'search_cancel_changes_nothing',
    'search_copy_of_several_folders_is_refused',
    'search_delete_asks_once_for_every_folder',
    'search_empty_query_is_refused',
    'search_enter_shows_it_in_its_folder',
    'search_finds_below_the_folder',
    'search_list_is_shown',
    'search_rows_show_the_folder_and_the_name',
    'search_starts_from_the_folder_on_screen',
    'search_wrong_query_is_refused',
    'word_motion_typeahead_is_preserved',
})


def conpty_diagnostics(case):
    records = []
    for line in (case.findtext('system-out') or '').splitlines():
        match = re.fullmatch(
            r'CONPTY wait_failed reason=(timeout|input_write|capture|child_exit) '
            r'elapsed_ms=([0-9]{1,12}) input_bytes=([0-9]{1,12}) '
            r'output_bytes=([0-9]{1,12}) parser=([0-4]) win32=([0-9]{1,10})', line)
        if match:
            records.append(dict(reason=match[1], elapsed_ms=int(match[2]),
                                input_bytes=int(match[3]), output_bytes=int(match[4]),
                                parser=int(match[5]), win32=int(match[6])))
        else:
            write = re.fullmatch(r'CONPTY input_write_failed win32=([0-9]{1,10}) '
                                 r'sent=([0-9]{1,12})', line)
            if write:
                records.append(dict(reason='input_write', win32=int(write[1]),
                                    input_bytes=int(write[2])))
            match = re.fullmatch(r'(FAIL|NOT RUN) check=([A-Za-z0-9_]{1,96}) '
                                 r'(?:rep=[0-9]{1,6}|reason=prerequisite_failed)', line)
            if match and match[2] in CONPTY_CHECKS:
                records.append(dict(status=match[1], check=match[2]))
        if len(records) == 32:
            break
    return records


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
        item = dict(name=name, status=status, seconds=seconds)
        if status == 'FAIL':
            diagnostics = conpty_diagnostics(case)
            if diagnostics:
                item['diagnostics'] = diagnostics
        tests.append(item)
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
