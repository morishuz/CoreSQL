# CoreSQL provenance

CoreSQL is independently maintained and is not affiliated with or endorsed by
SQLite or Hwaci. The C++20 implementation was introduced as an independent
architecture experiment, originally under `cpp/`. It has no SQLite runtime
dependency. Original CoreSQL contributions are covered by [LICENSE](LICENSE)
to the extent those rights exist. Record any future source adaptations and
upstream integrations here with their exact origins.

## Original reference baseline

- Upstream project: SQLite; official Git mirror: https://github.com/sqlite/sqlite
- Canonical Fossil repository: https://sqlite.org/src
- Imported branch: master, on 2026-09-08
- Version: 3.54.0 development source, not a stable release
- Git commit: `f3b9f74d81132426dee1ccc07a67fdad2ccfeaa9`
- Commit date: 2026-09-04T14:20:44Z
- Fossil check-in: `4021369bc9558fbfcfa83ee4cd6b986734b8c41b6d72bd97064e566ca8189ea8`
- Preserved local tag: `coresql-upstream-baseline-2026-09-08`

The initial CoreSQL commit changed documentation and licensing scope only.
The SQLite implementation, tests and build tooling remained unchanged through
the repository reorganization. The upstream history and pinned tag remain in the original development
repository, not in the ancestry of the new single-commit main branch.
Original source notices are retained in the current tree.

## Repository reorganization — 2026-09-13

The active contents of `cpp/` were promoted to the repository root. Current
contracts and historical research were separated into `docs/`; inherited SQLite
implementation and build files were removed from the active tree. The last
commit before the move is `7fe7e1a054`. Git history was not rewritten and no
repository visibility change is part of this reorganization.

The [reference manifest](benchmarks/reference/sqlite.json) pins the external
SQLite archive by revision and SHA-256. The setup script verifies both the
archive and the benchmark harness; it does not require a full clone or the
preserved tag. The unchanged `test/speedtest1.c` in that external source is
compiled for optional benchmark adapters. Normal CoreSQL builds do not fetch,
compile or link SQLite.

The five unchanged SQLLogicTest files and their original copyright notice remain
under `tests/sqllogictest/upstream/`. Their separate
[manifest](tests/sqllogictest/manifest.json) records origins and hashes. Historical
benchmark comparisons that explicitly rebuild old CoreSQL commits still require
those commits; current benchmark setup does not.

## Source-only main branch — 2026-09-16

At the owner's request, main is consolidated into one root commit for copying
into a separate public repository. The complete previous history, including the
release-audit fixes, is preserved on `codex/backup/pre-public-squash-2026-09-16`
in the original development repository. The pre-audit main tip was
`3e7b2f23d8c438ceb21b0fc54e8322aff43659b5`.

Historical revision identifiers in benchmark evidence refer to that archive;
they are not ancestors of the new main. Reference manifests, source fingerprints,
original notices and the scoped CoreSQL license are preserved. Historical scripts
that rebuild old revisions need the archive; normal builds do not. Consolidation
does not change authorship or license scope. No repository visibility change or
public release is part of this operation. Copy only main into the new repository,
not the backup branch or inherited tags.

## TPC-H readiness audit

`benchmarks/tpch/manifest.json` identifies the public DuckDB query revision and
hashes used for a syntax/binding readiness assessment. Query text is downloaded
separately into user-selected scratch space and is not vendored here. The small
diagnostic schema is derived from the column/type declarations in the pinned
DuckDB `extension/tpch/dbgen/dbgen.cpp`, with the explicit type substitutions
recorded in the manifest. It is not the benchmark schema or a data generator.
See [the readiness assessment](benchmarks/tpch/README.md) for source links,
limitations and the distinction from official TPC-H results.

The optional generated-data tools additionally pin the DuckDB Python package and
TPC-H extension to 1.5.5 (engine revision `d8cdaa33fd`). Generated CSV data stays
outside this repository; its manifest records row counts and hashes, plus engine
and extension binary hashes. The reference executes the separately pinned query
text unchanged. These tools and the DECIMAL implementation are original CoreSQL
code; no DuckDB engine or generator source is vendored or linked into CoreSQL.

The derived-relation/CTE and grouped-IN fixtures in `benchmarks/tpch/check_relations.py`
are original synthetic test data with explicit expected answers. They execute the
same checksum-pinned upstream queries without storing or rewriting those queries.

The original `benchmarks/tpch/sqlite_adapter.py` applies explicit, bounded
DATE/EXTRACT/SUBSTRING and derived-column-alias dialect substitutions to the checksum-pinned query text at
runtime and maps DECIMAL inputs to approximate REAL for SQLite. Adapted queries
are fingerprinted in performance reports, not vendored. SQLite itself is built
unchanged from the existing verified reference amalgamation and is not linked
into CoreSQL. The comparison documents its distinct precision and driver profile.
