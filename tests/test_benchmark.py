"""Check benchmark output and CLI behavior without timing thresholds.

Run through CTest (benchmark_schema), or pass the bench_core executable directly.
Importing this module during unittest discovery does not run the benchmark.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def output_hashes(directory):
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.iterdir()
    }


def check_benchmark(executable):
    workspace = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='bench-schema-', dir=workspace / 'out') as area:
        root = Path(area)
        fixture = root / 'fixture'
        fixture.mkdir()
        (fixture / 'a.txt').write_bytes(b'a')
        (fixture / 'b.txt').write_bytes(b'b')
        output = root / 'results'
        command = [str(executable), '--output', str(output), '--samples', '3',
                   '--enumerate', str(fixture)]
        subprocess.run(command, check=True, timeout=120)
        report = json.loads((output / 'summary.json').read_text(encoding='utf-8'))
        assert report['schema'] == 1
        assert report['samples'] == 3 and report['warmup'] == 3
        with (output / 'samples.csv').open(newline='', encoding='utf-8') as samples:
            records = list(csv.DictReader(samples))
        assert len(records) == len(report['results']) * 3
        for result in report['results']:
            assert (0 <= result['p50_us'] <= result['p95_us'] <=
                    result['p99_us'] <= result['max_us']), result['metric']
        idle = next(r for r in report['results'] if r['metric'] == 'vt_idle_generation')
        assert idle['output_bytes'] == 0
        enumeration = next(r for r in report['results'] if r['metric'] == 'enum_win32_read_only')
        assert enumeration['items'] == 2 and enumeration['output_bytes'] == 0

        # A rejected rerun must leave the existing result files byte-identical.
        before = output_hashes(output)
        assert subprocess.run(command, timeout=10).returncode != 0
        assert before == output_hashes(output)
        for value in ('0', '-1', '502', 'bad'):
            bad = root / ('bad-' + value)
            result = subprocess.run(
                [str(executable), '--output', str(bad), '--samples', value], timeout=10)
            assert result.returncode != 0, value
            assert not bad.exists(), value
        assert (fixture / 'a.txt').read_bytes() == b'a'
        assert (fixture / 'b.txt').read_bytes() == b'b'
    print('Benchmark schema, percentiles, read-only enumeration, no-clobber and CLI validation: PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path, help='path to bench_core')
    args = parser.parse_args()
    check_benchmark(args.executable.resolve(strict=True))


if __name__ == '__main__':
    main()
