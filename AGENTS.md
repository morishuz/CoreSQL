# CoreSQL contributor guidance

CoreSQL is an independent experimental C++20 embedded database. The active
implementation lives at the repository root. Read README.md, docs/guide.md and
docs/architecture/direction.md for its contract and priorities.

- Favor small, maintainable modules, RAII and standard-library containers.
  Errors use coresql::Error. Format changed C++ with .clang-format.
- Keep SQL parsing/coercion outside the core and domain types behind add-on
  interfaces. Preserve useful runtime memory diagnostics.
- Preserve atomic mutations, snapshot isolation, savepoints and durability.
  Snapshot export is not a durable transaction commit.
- Build with `cmake -S . -B build/dev`, `cmake --build build/dev -j 4`, and
  `ctest --test-dir build/dev --output-on-failure`. Run meaningful engine tests
  with `-DCORESQL_SANITIZERS=ON` in a separate Debug build.
- SQLite is a pinned, external test/benchmark reference, not the implementation.
  Use benchmarks/reference/prepare.py and follow benchmarks/README.md. Do not
  weaken correctness or durability to claim performance improvements.
- Preserve original notices in third_party/ and tests/sqllogictest/upstream/.
  CoreSQL's LICENSE covers its original contributions; do not assert ownership
  over inherited material. Record adaptations in PROVENANCE.md.
- Do not change repository visibility or rewrite published history without
  explicit owner approval.
- Keep temporary audit notes, profiles and intermediate benchmark results out of
  the tracked source tree. Public documentation should describe the product, its
  contracts, reproducible methods or required attribution. Do not publish internal
  review transcripts, refactoring diaries, agent handoffs or local workspace notes.
  Audit documentation and artifacts for public relevance before committing or pushing.

See CONTRIBUTING.md for build options, installation checks and CI.
