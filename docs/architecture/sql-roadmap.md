# SQL backlog

The [SQL contract](../contracts/sql.md) is the source of truth for implemented
syntax and semantics. This page records possible next work, not promised scope
or a release schedule. Choose features from real application requirements.

Derived tables, ordinary CTEs, DATE/DECIMAL, SQL savepoints, DROP TABLE/INDEX,
table/column rename, OFFSET, generated integer primary keys and column-only
INSERT RETURNING are implemented. The pinned 22-query analytical workload has
[recorded correctness and timing evidence](../../benchmarks/tpch/README.md);
that does not imply full SQL compatibility or production readiness.

## Candidates

| Area | Remaining work |
| --- | --- |
| Application integration | Named parameters, streaming result access, expressions in RETURNING and UPDATE/DELETE RETURNING |
| Data integrity | Table-level composite key declarations, CHECK, foreign keys and explicit ON CONFLICT/UPSERT |
| SQL convenience | Remaining projection forms, NULLS FIRST/LAST and broader scalar/string functions; check the dialect contract before selecting a form |
| Advanced queries | Recursive CTEs, windows and persisted views |
| Generated keys | Maintained/index-assisted allocation to avoid the initial MAX scan; sequences or AUTOINCREMENT require separate semantics |
| Types and indexes | BLOB, collations, broader domain operations and partial/expression indexes |

Foreign keys, triggers and views require dependency and lifecycle design, not just
parser additions. General SQLite affinity quirks, its file/C ABI and every PRAGMA
are not compatibility targets. Server protocols and users/roles are outside the
current embedded-library scope. Paging, concurrent owners and Windows are separate
platform/storage work, not SQL syntax milestones.

## Acceptance criteria

For each feature, specify a concrete application example, exact NULL/type/error
semantics, module boundaries and maintenance cost. Check successful and rejected
forms, empty inputs, duplicates, parameter binding and relevant interactions.
Writes and schema changes need statement atomicity, savepoint/snapshot and durable
reopen coverage. Use SQLite as an oracle only where the documented contracts agree.

Run relevant differential and sanitizer checks and update the contract when a
feature lands. Completing a parser rule alone does not complete the feature.
