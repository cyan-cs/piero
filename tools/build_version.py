"""Canonical build versions supported by the updater's bounded SemVer parser."""
import re
import sys

NUMBER = r'(?:0|[1-9][0-9]*)'
IDENTIFIER = rf'(?:{NUMBER}|[0-9]*[A-Za-z-][0-9A-Za-z-]*)'
VERSION = re.compile(
    rf'({NUMBER})\.({NUMBER})\.({NUMBER})'
    rf'(?:-{IDENTIFIER}(?:\.{IDENTIFIER})*)?'
    r'(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?')


def valid(version):
    if len(version) >= 64:
        return False
    match = VERSION.fullmatch(version)
    return bool(match and all(int(part) <= 0xffffffff for part in match.groups()))


if __name__ == '__main__':
    if len(sys.argv) != 2 or not valid(sys.argv[1]):
        print('Invalid build version: use SemVer with at most 63 ASCII characters and 32-bit core numbers')
        raise SystemExit(1)
