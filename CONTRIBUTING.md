# Contributing to CoreSQL

CoreSQL values a small, maintainable implementation with explicit behavior and
measured tradeoffs. Read the [guide](docs/guide.md),
[project direction](docs/architecture/direction.md) and [agent guidance](AGENTS.md).

## Development

Requirements: C++20 with floating-point `std::from_chars`/`std::to_chars`,
CMake 3.20+, and Python 3.9+ for the full corpus tests. CMake checks the standard
library at configuration time. CI selects Xcode 26.6 on macOS 26. Apple's floating-point parser requires the
macOS 26 runtime; selecting a newer SDK on macOS 15 is not sufficient. The selected version
is listed in the [hosted runner inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md).
Select a suitable local Xcode using `DEVELOPER_DIR` before configuring a fresh
build directory; a newer language flag cannot add missing standard-library APIs.
macOS 26 and Linux are CI targets. Storage uses POSIX APIs; Windows is not supported.
CI results establish platform coverage; they do not establish production readiness.

```sh
cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev -j 4
ctest --test-dir build/dev --output-on-failure -j 4

cmake -S . -B build/check -DCMAKE_BUILD_TYPE=Debug -DCORESQL_SANITIZERS=ON
cmake --build build/check -j 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/check --output-on-failure -j 2
```

For the configured-size boundary regression, also run a small-limit build:

```sh
cmake -S . -B build/small-limit -DCMAKE_BUILD_TYPE=Release -DCORESQL_MAX_ENCODED_MIB=8
cmake --build build/small-limit --target coresql_encoded_limit_test -j 4
ctest --test-dir build/small-limit -R '^encoded_size_limit$' --output-on-failure
```

The default suite checks library/header agreement and persistence; the small-limit
run additionally checks oversized-commit rejection and continued owner usability.

Keep regression tests meaningful: check behavior and invariants, not implementation
shape. Do not rewrite upstream test fixtures to match the engine. Run relevant
sanitizer checks for engine changes. Format changed C++ with `.clang-format` and
run `git diff --check`; avoid unrelated formatting changes.

Core code must not depend on SQL parsing or know concrete optional domain types.
Preserve atomicity, evaluation order, NULL behavior, borrowed-value lifetimes,
query diagnostics and durability. Snapshot export does not commit durably.

## Benchmark reference

Follow [the reference setup](benchmarks/reference/README.md). Optional comparisons
use a pinned, unmodified SQLite baseline; keep compiler, workload and durability
settings explicit. Do not present configuration gains as engine improvements.
Published benchmark evidence stays with its methodology; historical investigations
and intermediate observations remain in Git history. Current comparisons work without full Git history.

## Installation and consumer verification

```sh
cmake -S . -B build/package -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/package -j 4
python3 tests/package/check.py --build build/package
```

The check installs to a temporary prefix, moves it, builds a core-only consumer
and a consumer of every add-on, and verifies version/component rejection. Package
exports follow [CMake's relocatable package support](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html).

The CI workflow uses macOS and Ubuntu, runs Release and ASan/UBSan suites with the
pinned oracle, and checks production package consumption. Workflow dependencies
are pinned to reviewed commits; update those pins deliberately.

## Changes and release preparation

Explain the problem, changed behavior, relevant validation and known limitations
in proposed changes. Keep new dependencies and feature flags justified. Original
contributions fall under [LICENSE](LICENSE); preserve all external notices and
record copied/adapted source in [PROVENANCE.md](PROVENANCE.md).

Before publishing an experimental release, check clean-clone builds, CI results,
installed consumption, package version and changelog, current contracts, fixture
hashes, source notices and benchmark reproducibility. Tag the tested commit only
after deciding to publish. Do not treat the historical SQLite VERSION or tags as
CoreSQL release versions; CMakeLists.txt is the CoreSQL version source.

The single-commit main branch is prepared for a separate public repository.
Original development history is preserved on `codex/backup/pre-public-squash-2026-09-16`
in the development repository. Keep provenance and notices when copying main.
Creating the public repository and changing visibility remain owner decisions;
do not copy backup branches or inherited SQLite tags into the new repository.
