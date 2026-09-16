#!/usr/bin/env python3
"""Inspect pinned TPC-H SQL readiness on an empty, simplified schema; no timing."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "sqllogictest"))
from run import Core


def load_queries(directory, fetch=False):
    manifest = json.loads(Path(__file__).with_name("manifest.json").read_text())
    queries = []
    # Verify the complete query set before starting the engine. Existing files
    # are never overwritten; changing the upstream pin is an explicit review.
    for item in manifest["queries"]:
        path = directory / item["file"]
        if path.exists():
            data = path.read_bytes()
        elif fetch:
            url = (f'https://raw.githubusercontent.com/duckdb/duckdb/{manifest["revision"]}/'
                   f'{manifest["query_directory"]}/{item["file"]}')
            with urlopen(url, timeout=30) as response:
                data = response.read(1024 * 1024 + 1)
        else:
            raise RuntimeError(f"Missing query {path}; supply --fetch or a local pinned copy")
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise RuntimeError(f"Query checksum mismatch: {path}")
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as output:
                output.write(data)
        queries.append((item["file"], data.decode("utf-8")))
    return manifest, queries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--fetch", action="store_true", help="Fetch missing pinned queries")
    parser.add_argument("--output", type=Path, help="Write JSON evidence in addition to summary")
    args = parser.parse_args()
    manifest, queries = load_queries(args.queries, args.fetch)
    results = []
    for name, sql in queries:
        core = Core(str(args.bridge.resolve()))
        try:
            for ddl in manifest["diagnostic_schema"]:
                status, detail = core.execute(ddl)
                if status != "ok":
                    raise RuntimeError(f"Diagnostic setup failed: {ddl}: {detail}")
            status, detail = core.execute(sql)
            results.append({"query": name, "status": status, "detail": detail})
        finally:
            core.close()
    report = {
        "purpose": manifest["purpose"],
        "duckdb_revision": manifest["revision"],
        "bridge_sha256": hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
        "diagnostic_schema": manifest["diagnostic_schema"],
        "results": results,
    }
    for result in results:
        print(f'{result["query"]}: {result["status"]}: {result["detail"]}')
    print(f'{sum(r["status"] == "ok" for r in results)}/{len(results)} executed on the diagnostic schema; '
          'this is not a correctness or performance score.')
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    # Missing features are evidence, not failure of the audit tool. Crashes,
    # checksum failures and setup errors remain nonzero exits.


if __name__ == "__main__":
    main()
