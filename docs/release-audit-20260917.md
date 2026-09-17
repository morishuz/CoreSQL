# SQL type modularization: public source audit

September 17, 2026. Scope: the pending changes on top of
`d4982285738cf609bb27804cbb76ad760aec7753`, before committing and pushing to
`origin/main`. CoreSQL remains a 0.1.0 experimental source preview. This audit
does not create a release tag, change repository visibility, or establish
production readiness.

## Outcome

No unresolved code, packaging, provenance or distribution blocker was found in
this focused review. The changes are suitable to commit as an experimental
source update. One existing repository-level readiness gap remains: private
vulnerability reporting is disabled and there is no `SECURITY.md` reporting
route. Configure a real private reporting channel before treating public release
preparation as complete; no contact details or response commitments were invented.

## Findings addressed

- Correct the extension contract's obsolete statement that repeatability probes
  may omit argument types. The implementation now uses typed dispatch, including
  correlated scopes.
- Correct the public SQL header's default-registration comment to include VECTOR.
- Add the custom-type walkthrough to the documentation index and distinguish the
  historical release-readiness report from this current audit.
- Clarify that benchmark source hashes identify the measured snapshot, preceding
  the audit's public-header comment and documentation corrections. Raw benchmark
  observations and hashes are preserved.

## Review and distribution checks

- Reviewed adapter registration, configuration snapshots, conversion ambiguity
  and result validation, scalar policy, typed operation dispatch, vector parsing,
  NULL handling, storage conversion, nested-query analysis and packaging changes.
- All eleven backend source relocations are byte-for-byte identical to their
  originals. Domain encodings and backend implementations are unchanged.
- No license, provenance notice, third-party source, upstream test fixture or
  workflow was changed. All six pinned SQLLogicTest fixture/notice hashes match.
- Pattern-based scans found no private keys, common credential-token patterns,
  local user/volume paths, conflict markers or unexpected large files in the
  source distribution. This is a hygiene check, not a comprehensive security audit.
- Local links in changed Markdown resolve. Source/binary compatibility remains
  experimental; the default SQL installer now also installs VECTOR, as documented.
- The existing benchmark evidence remains qualified: SF 0.03, macOS supervisor,
  single-thread in-memory client latency, three samples after warmup, exact answer
  checks against pinned DuckDB, and explicit resource limits. No official TPC-H
  score or broad performance guarantee is claimed.

## Validation

The clean source build was exported from the staged Git index, without `.git`,
previous build outputs or CMake caches. Host: Apple M1, macOS 27.0, Apple Clang,
C++20 Release configuration.

| Check | Result |
| --- | --- |
| Clean staged-source Release build and tests | 68/68 passed |
| Clean-build installed/relocated package | Core, all-add-on and custom-type consumers; version/component rejection; persistence and backup/restore passed |
| Clean staged-source 8 MiB encoded limit | Persistence, rejection and continued-usability regression passed |
| Full Debug ASan/UBSan suite from preceding implementation validation | 68/68 passed; audit changed no executable code |
| Production BUILD_TESTING=OFF package from preceding validation | Installed consumption and persistence checks passed |
| Full repaired Q1–Q22 run | 22/22 match; Q9 memory regression resolved |
| Q17 follow-up | Nine samples: 13.125 ms fixed vs 12.994 ms baseline; execution counters identical |
| Whitespace and fixture integrity | Passed |

See [the measured fixes and raw results](../benchmarks/tpch/results/TYPE_MODULARIZATION_FIXES_20260917.md)
for per-query times and provenance. Local testing does not replace hosted testing
of the new commit.

## Repository and hosted CI

Read-only GitHub checks confirmed that `morishuz/CoreSQL` is already public and
its default branch is `main`. Local and remote main matched at audit start.
Visibility and security settings were not changed.

The old account-level CI restriction is no longer blocking the current main:
[the baseline CI run succeeded](https://github.com/morishuz/CoreSQL/actions/runs/35143463682).
The new commit's existing macOS/Linux Release and sanitizer workflow must run
after the push. This document records the pre-commit audit, not a prediction of
that run's outcome. No backup branches or inherited tags are part of the push.
