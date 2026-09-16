# Experimental release readiness

Second audit: September 16, 2026, starting from `3e7b2f23d8` and including the
configuration fix below. CoreSQL 0.1 remains an experimental source preview,
not a production-ready or cross-platform-validated release. Installation,
backup and upgrade instructions are in [usage.md](usage.md).

## Findings

**P1 — hosted platform validation remains blocked.** The inspected
[workflow on the pre-audit main](https://github.com/morishuz/CoreSQL/actions/runs/35128770024)
had four macOS/Linux Release/sanitizer jobs with no executed steps. The checked
annotation reports an account-level runner-access restriction, not a build or
test failure. Restore runner access and obtain a green
[CI matrix](../.github/workflows/ci.yml) on the final revision. Local macOS testing
does not establish Linux coverage. The new public repository will need its own CI
run; the existing private workflow link may be inaccessible to public readers.

**P2 — non-default encoded-size mismatch fixed in this audit.** CMake previously
passed `CORESQL_MAX_ENCODED_MIB` privately to the library, leaving tests that include
[storage.hpp](../src/storage.hpp) with its 1024 MiB fallback. A non-default build
therefore violated the one-definition rule for the inline limit constant.
The definition now propagates to in-tree consumers through the build interface;
it is not exported as an installed consumer requirement. The
[new regression](../tests/encoded_limit_test.cpp) checks agreement with the compiled
library, persistence, oversized-commit rejection, subsequent writes, checkpoint,
backup and reopening. An 8 MiB run passed, and deliberately omitting the propagated
definition made the test fail. The [contributor guide](../CONTRIBUTING.md) includes
the 8 MiB check. Adding it as a separate hosted CI configuration was deferred:
the current OAuth token cannot push workflow changes. The existing workflow is
unchanged; its default suite picks up the new test at the default size limit.

**Public reporting route remains to be configured.** Before publishing the new
repository, provide a real private vulnerability-reporting channel and document
it in SECURITY.md. No contact address, enabled GitHub feature or response SLA has
been invented. This is an owner/repository setup task, not an engine test failure.

**Benchmark tooling has a platform boundary.** The TPC-H timing supervisor needs
a Git checkout and macOS `sysctl`/process tools for metadata and monitoring. Native
builds, tests and package installation work without Git history. The benchmark
instructions now distinguish these requirements; portable TPC-H measurement is
not claimed. No fresh performance comparison was run in this audit.

## Validation

Fresh builds use a source-only copy in a new temporary directory, without `.git`,
prior CMake caches or build outputs. Environment: macOS arm64, Apple Clang 21
Command Line Tools with macOS 26.5 SDK; default encoded limit 1024 MiB unless noted.

| Check | Result |
| --- | --- |
| Fresh Release build and suite | 63/63 passed (28.28 s) |
| Fresh Debug ASan/UBSan build and suite | 63/63 passed (166.13 s), halt-on-error enabled |
| 8 MiB configured limit | Persistence/rejection regression passed; library and test compile definitions agree |
| Regression sensitivity | Deliberately mismatched test/library configuration rejected |
| Production build, BUILD_TESTING=OFF | Passed; no storage fault-hook or SQLite symbols in core archive |
| Installed/relocated package | Core and all-add-on consumers, version/component rejection, installed persistence and backup/restore passed |
| Distribution isolation | Builds and tests ran from a fresh source copy without Git metadata or previous caches |
| Documentation and evidence | 117 local links resolve; 22 README timings match unchanged raw evidence; diff whitespace check clean |

The default suite includes SQLLogicTest, SQL differential tests, generated IDs,
transaction/savepoint behavior, schema evolution, backup/restore, recovery and
legacy fixtures. Process-interruption tests do not establish physical power-loss
resilience. The large-storage test uses 72 MiB of payload, not a full-capacity
1 GiB or 16 GiB certification. Optional standalone SQLite benchmark executables
are not part of these local suite runs.

Review scope: storage publication and recovery, statement atomicity, generated
IDs, transaction/savepoint ownership, build configuration, installed consumption,
workflow setup, documentation and source-distribution hygiene. No further concrete
engine defect was established by this focused review; this is not an exhaustive
correctness or security audit. Known limits include RAM-resident data, trusted
native extensions, one file owner, external serialization, initial maximum scans
for generated IDs and experimental source/file compatibility.

The SQLLogicTest inputs and original copyright match their pinned hashes. A
current-tree scan for private-key headers and common credential token formats
found no matches; this does not certify the absence of all sensitive material.
The public tree retains the scoped license, external notices, provenance and
unchanged raw evidence for the README's historical benchmark.

## History and publication

The owner authorized a single root commit on main and a complete history backup
on `codex/backup/pre-public-squash-2026-09-16` in the original development repository.
The backup includes the audit fixes; the pre-audit tip remains its ancestor.
The squashed main and backup tip have identical trees. See
[provenance](../PROVENANCE.md#source-only-main-branch--2026-09-16).

The original repository stays private. No public repository, release tag or
visibility change is created by this audit. Copy only main to the separately
created public repository; do not mirror backup branches or inherited SQLite tags.
Historical commit IDs in retained evidence refer to the development archive,
not to ancestors of the new main. The backup intentionally retains that history
and its storage cost in the development repository.

Before a tagged release, restore hosted validation, configure the reporting route,
record the tested final revision/toolchains/limit, and finalize version and
changelog. Keep experimental status and the backup/upgrade policy explicit.
