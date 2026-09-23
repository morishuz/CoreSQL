# Application workload baseline

This optional in-process SQL harness compares CoreSQL and the pinned SQLite
reference on deterministic state-store and event-store operations. Both engines
receive the same schema, SQL, parameters and transaction boundaries. It is a
closed-loop, single-client workload, not an application trace or a concurrency,
real-time, power-loss or endurance certification.

```sh
python3 benchmarks/reference/prepare.py --output /tmp/coresql-reference
cmake -S . -B build/compare -DCMAKE_BUILD_TYPE=Release -DCORESQL_BENCHMARKS=ON \
  -C /tmp/coresql-reference/reference.cmake
cmake --build build/compare --target coresql_application_benchmark -j 4
python3 benchmarks/application/run.py --binary build/compare/coresql_application_benchmark \
  --output /tmp/application-results --operations 1000 --runs 3
```

Use a new output directory. Results include per-operation CSV, latency distributions,
source/binary fingerprints, host metadata and a comparison. The runner alternates
engines in fresh processes and sets `TMPDIR` to the output directory. Record its
filesystem/device and keep other benchmarks, builds and profilers idle during timing.
Keep measurements outside the tracked source tree. Revision metadata requires a
Git checkout; the executable also works from a source archive.

Build options are recorded from the neighboring CMake cache. Keep `BUILD_TESTING`
consistent across comparisons; `OFF` excludes engine diagnostic counters.

## Fixture and operations

The fixture contains 100 devices and configurable event rows with INTEGER primary
keys, a device ID, increasing INTEGER timestamp, INTEGER value and fixed-size TEXT
payload. Indexes cover `(device,ts)` and `ts`. Payload contents and query parameters
are deterministic; every read is checked against an arithmetic oracle.

Pass `--covering-index` to the runner (or append `covering` to the executable's
arguments) to replace `(device,ts)` with `(device,ts,id)` in **both engines**.
This optional schema variant allows CoreSQL to return event IDs and timestamps
entirely from the index. Keep its results separate from the default schema;
compare memory, ingestion/deletion, checkpoint and reopen costs as well as reads.
The manifest records the variant. Secondary CoreSQL indexes do not implicitly
include primary-key columns.

Final contents are checked with a single streaming pass after each write phase, then integrity
and persistent reopening are checked. Those checks are outside timed intervals.
They access data and therefore affect subsequent cache state.

- Primary-key reads return a value and payload, with changing keys.
- Keyset event pages and latest-event queries return up to 20 IDs/timestamps.
- Time-range aggregates count and sum 100 consecutive events.
- Full-table aggregates count and sum the seeded events, exercising sequential scans.
- Small joins fetch ten events with their device labels.
- Single-row updates and changed-existing/new-key UPSERT each have their own
  transaction. A separate `upsert_unchanged` phase reapplies the current value; it
  measures no-op handling and must not be described as a changed-row write.
- Ingestion appends 16 events per transaction; retention deletes successive
  16-event timestamp ranges, stopping before half the seed data is removed.
- Mixed transactions issue eight reads of different keys and two existing-row
  updates. Persistent runs checkpoint synchronously every 50 mixed transactions.
  Mixed total latency includes that maintenance; checkpoint samples are also
  emitted separately. Reopening is measured three times after the write phases.

Statements are prepared before measured phases; CoreSQL's optional SQL template
cache is disabled. CoreSQL still lowers/binds each execution, whereas SQLite
reuses compiled statements. Timings include normal SQL execution and owned result
construction. Read parameters and expected results are built outside timing;
write parameter construction occurs inside the application transaction on both
engines. Each read phase has one unmeasured warmup. Writes have no hidden warmup.
Setup uses 256-row commits, then builds indexes, checkpoints and reopens persistent
files. The setup sample includes all of that work and is not a query latency.

## Durability and memory

Persistent CoreSQL uses its ordinary synchronized commit protocol. SQLite uses
WAL, `synchronous=FULL`, `fullfsync=ON`, `checkpoint_fullfsync=ON`, disabled automatic
WAL checkpoints and disabled mmap. The harness checks the durability PRAGMA values.
Both acknowledge synchronized commits, subject to the filesystem/device honoring
them. Their log formats, sync counts and automatic maintenance rules differ.
CoreSQL can additionally trigger its own log-size checkpoint. SQLite explicit
maintenance uses a checked TRUNCATE WAL checkpoint, not VACUUM.

Default scenarios:

| Scenario | Rows | Payload bytes/row | CoreSQL clean cache | SQLite page cache |
| --- | ---: | ---: | ---: | ---: |
| memory-small | 10,000 | 128 | Resident | 64 MiB target |
| memory-large | 100,000 | 1,024 | Resident | 64 MiB target |
| durable-small | 10,000 | 128 | Resident | 64 MiB target |
| durable-cache-pressure | 100,000 | 1,024 | 4 MiB target | 4 MiB target |

The large fixture has about 98 MiB of payload. Cache-pressure testing exceeds the
configured cache, **not necessarily physical RAM**. CoreSQL's decoded cache excludes
indexes, dirty chunks, metadata and query buffers. SQLite caches encoded pages;
equal numeric cache settings do not imply equal total memory. OS filesystem caches
are not flushed. In-memory SQLite cannot evict its database to a durable file.

CSV reports current and peak RSS outside timed samples. Peak RSS includes setup,
verification and earlier phases. File bytes exclude unlinked scratch files and can
miss transient checkpoint peaks. CoreSQL I/O counters count submitted bytes, not
physical device traffic; they reset on reopen. First-to-last sample differences
exclude the first operation.

`*_execute`, `*_commit` and total samples overlap: do not sum them. Throughput uses
summed timed intervals, excluding verification and measurement gaps. Read units
are queries, write units are affected/attempted rows, and mixed units are ten SQL
statements. Compare like phases. Reports use nearest-rank percentiles, omitting
p95 below 20 samples and p99 below 100; these are estimates, not latency bounds.

## Profiling

The executable can isolate a phase after identical setup:

```sh
CORESQL_APPLICATION_PROFILE=1 ./build/compare/coresql_application_benchmark \
  coresql memory 100000 1024 100000 0 /tmp point_read
```

Attach a profiler after `PHASE_READY point_read` appears on stderr. Phase names
are in `main.cpp`; `all` runs the default sequence. Profiling suppresses CSV, RSS
and file-stat reporting but retains verification and parameter generation. Keep
profiles private and separate from timings. After forcibly stopping a process,
remove its scratch directory only once it has exited.

## Background checkpoints

Pass `--background-checkpoints --phase mixed` to `run.py` to use background
maintenance in both engines. The binary accepts the trailing `background` flag.
Requests occur every 50 mixed transactions. One job runs at a time, overlapping
requests are coalesced, and every future is observed for errors. CoreSQL enables
concurrent-safe callbacks; SQLite uses a separate checkpoint connection with
FULL/fullfsync/checkpoint_fullfsync and TRUNCATE checkpoints. SQLite durable write
transactions use BEGIN IMMEDIATE in both modes so checkpoint contention is
included in latency rather than failing a deferred read-to-write upgrade.

`checkpoint_request` measures request/poll overhead; `maintenance_drain` measures
the final wait. `write_workload_wall` includes verification, measurement gaps and
the drain. Stderr reports requested/started/completed/coalesced jobs and SQLite
busy retries. CoreSQL maintenance durations include nested I/O: do not sum them.
Compare completed checkpoint counts as well as transaction latency.

`--adaptive-checkpoints` (binary flag `adaptive`) checks every 50 mixed transactions
for log growth above half the initial compacted file size (minimum 1 MiB), or
nonzero growth after one second. This benchmark policy uses the same background
scheduler; it does not change engine defaults. CoreSQL's retained-history bound
remains active; its automatic-checkpoint counter reveals foreground fallbacks.

## Transaction batching

Use `--phase batch_updates` to compare 1, 4, 16 and 64 changed-row updates per
transaction. Each phase executes `--operations` batches; larger batches therefore
update more rows. This family is excluded from `all`. Contents are verified after
each phase, and durable runs verify reopening.

`batch_update_N` measures acknowledgement of the entire atomic batch; `units` is
N rows. Report batch percentiles separately from amortized milliseconds per row.
Durable phases finish with a separately timed `_maintenance` checkpoint; `_wall`
also includes verification and measurement gaps. Batches are ready at submission,
so timings exclude application queueing. Both engines retain ordinary synchronized
commits; this benchmark adds no engine group commit.
