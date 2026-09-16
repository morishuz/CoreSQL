"""Collect opt-in CoreSQL test-build counters under the benchmark safety guards.

This is a diagnostic run, not a timing measurement or independent answer check.
Run measure.py separately with profiling disabled to validate and time results.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from measure import run
from audit import load_queries
from validate import dataset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--query', default='q19.sql')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pin, queries = load_queries(args.queries)
    if args.query not in dict(queries):
        parser.error('Unknown pinned query')
    manifest, digest = dataset(args.data)
    args.repeats, args.timeout, args.load_timeout, args.memory_mb = 1, 15, 120, 2048
    args.sqlite_library = None
    os.environ['CORESQL_PROFILE'] = '1'
    outcome = run(args, 'coresql', args.query)
    records = []
    for line in outcome.get('stderr', '').splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict) and 'candidate_pairs' in value:
            records.append(value)
    report = {'purpose': __doc__, 'status': outcome['status'], 'query': args.query,
              'scale': manifest['scale'], 'dataset_manifest_sha256': digest,
              'query_revision': pin['revision'],
              'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
              'limits': {'query_seconds': 15, 'load_seconds': 120, 'group_rss_mib': 2048}}
    if outcome['status'] == 'ok':
        if not records:
            raise RuntimeError('Bridge returned no counters; requires a profiling-capable test build')
        report['counters'] = records[-1]
    else:
        report['phase'] = outcome.get('phase')
        report['detail'] = outcome.get('stderr', '')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
