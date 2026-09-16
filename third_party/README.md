# External material and notices

The CoreSQL license is [LICENSE](../LICENSE). It does not replace notices for
external material or claim ownership of inherited code.

| Material | Location and purpose | Notice |
| --- | --- | --- |
| SQLite reference source | Downloaded separately for optional comparisons; original source also remains in Git history | [Preserved upstream license information](notices/SQLite.md), with source-file notices retained in the downloaded archive |
| SQLLogicTest corpus | Five pinned upstream files in `tests/sqllogictest/upstream/` | [Original copyright and license choices](../tests/sqllogictest/upstream/COPYRIGHT.md) |

`notices/SQLite.md` is the unchanged license-information document from the
pinned upstream baseline. Its references to a source tree and bundled build
scripts describe that SQLite archive, not the current CoreSQL tree. The SQLite
build archive contains additional notices (including its build tools); the
reference preparation script preserves them with the source.

See [PROVENANCE.md](../PROVENANCE.md) for the exact revisions. Generated reference
builds are not installed with the CoreSQL library.
