# Documentation

## Use CoreSQL

- [Guide and module map](guide.md)
- [Installation, persistence, backup and upgrades](usage.md)
- [Executable examples](../examples/README.md)
- [SQL dialect](contracts/sql.md)
- [Relational C++ API](contracts/relational.md)
- [Storage and durability](contracts/storage.md)

## Extend CoreSQL

- [Type, function, aggregate and index contracts](contracts/extensions.md)
- [Add a backend and SQL type](extensions/custom-type.md)
- [JSON key extraction](extensions/json.md)
- [Graph extension](extensions/graph.md)

## Contribute and evaluate

- [Contributing](../CONTRIBUTING.md)
- [Architecture](architecture/direction.md)
- [SQL backlog](architecture/sql-roadmap.md)
- [Benchmark methods and reproduction](../benchmarks/README.md)
- [Release checklist, open findings and validation evidence](release-readiness.md)
- [Provenance and external material](../PROVENANCE.md)

Historical research, review diaries and intermediate measurements remain in the
[development archive](../PROVENANCE.md#source-only-main-branch--2026-09-16), including
revision `e93b6b8904`. Current contracts describe supported behavior;
the retained benchmark report describes its dated measurement, not the latest code.
