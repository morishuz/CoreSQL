# Landmark-memory pilot

This pilot explores CoreSQL as a persistent, process-local memory for a robot.
It stores derived observations, not raw camera/video or LiDAR streams. The first
workload is replayable without hardware or a machine-learning runtime.

## Where it fits

1. An external perception component produces a descriptor and an estimated pose.
2. A background database worker accepts a bounded batch of observations.
3. It persists the batch in one synchronized transaction.
4. A retrieval request filters stored landmarks by descriptor model and position,
   computes squared L2 distances and returns the five nearest candidates.
5. The application verifies candidates using its own geometric/perception logic.

Database results must not directly command steering, braking or actuators. This
pilot does not implement perception, localization, sensor fusion, route planning
or a safety controller. Search and persistence have no hard real-time deadline or
worst-case latency guarantee. Keep database work off the control thread; deployment
requires application-specific scheduling, queue bounds and measurements on the
target hardware. Do not silently drop acknowledged observations when a queue fills.

## Current data and behavior

The executable creates a new directory containing `landmarks.core` and a verified
`backup.core`. It never overwrites an existing directory. Each landmark contains:

- A stable integer identity.
- A descriptor-model name; incompatible embedding spaces are never compared.
- Cartesian x/y coordinates in one application-defined map frame.
- A `seen` sequence field and a fixed 128-dimensional float32 descriptor.

The fixture uses deterministic synthetic descriptors and coordinates. Its matches
validate storage/query behavior; they say nothing about real-world recognition
accuracy. Production ingestion must record coordinate-frame and model-version
identities and obtain descriptors from the application. Multiple coordinate frames
must not be mixed using the fixture's single-frame schema.

The pilot verifies exact nearest candidates against an independent C++ distance
calculation, including deterministic ID ordering for ties. It also checks retained
snapshot visibility, durable reopening and native backup. Tests exercise invalid
vector dimensions, statement atomicity and rollback of a staged observation removal.
Vector search currently scans the filtered candidates; it is not an approximate
nearest-neighbor index. Measure its scaling before selecting an ANN implementation.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target coresql_landmark_memory -j 4
./build/release/coresql_landmark_memory /tmp/new-landmark-demo
ctest --test-dir build/release -R landmark_memory --output-on-failure
```

## Acceptance boundary

The initial engineering profiles use 1,000 and 10,000 stored landmarks; the
operational runner accepts up to 100,000. The database has one owner and calls are
serialized. Live data fits in memory, and checkpoints/backup run in the background
worker. No pager, cross-process readers, guaranteed memory ceiling, approximate
search recall or automotive certification is implied.

The existing contracts remain authoritative. Synthetic correctness tests are a
starting point for a real application: target hardware, sensor rate, allowed data
loss before acknowledgement, retention horizon, retrieval deadlines and queue
capacity must be specified before claiming operational fitness.

See the [operational measurement method](../../benchmarks/operational.md).

## Bounded worker

The selected milestone is a local map that fits in RAM: at most 100,000 live
128-dimensional landmarks, with a provisional 1 GiB process budget to validate
on deployment hardware. Paging and unbounded observation history are outside this
milestone. The budget is a target, not an allocator-enforced ceiling.

`examples/landmark_memory/worker.hpp` provides a concrete application worker in
addition to the standalone fixture. One thread owns a dedicated persistent file
and SQL connection. Observations carry explicit model-version and coordinate-frame
identities; retrieval requires both to match. Applications supply actual descriptors
and frame transforms. The default queue holds 32 pending requests (configurable
1–64), plus one active group of at most 16 requests by default. Ingest/erase
batches contain 1–32 items. Model and frame names are nonempty and at most 64 bytes;
coordinates/descriptors must be finite.

`try_ingest`, `try_search` and `try_erase` return an optional future. An empty optional
means no request was accepted; the caller retains its input and chooses whether
to retry or shed unacknowledged work. An accepted future reports its result or
exception. A successful ingest future is fulfilled only after synchronized commit.
If background maintenance fails fatally, queued requests receive that exception
and subsequent submissions rethrow it; a stopped worker never masquerades as a
temporarily full queue. Earlier acknowledged commits remain acknowledged.
At landmark capacity, replacement by stable ID remains possible, but a batch adding
too many identities fails atomically. Retention is explicit through `try_erase`;
there is no silent eviction of acknowledged observations. UPSERT preserves stable
row identity when updating an existing landmark.

Search returns at most five exact candidates. It accepts a stop token/deadline and
always applies a 10-million-unit query-work cap; callers can request less. Queued
searches retain their deadline, so waiting does not reset it. Ingest cancellation
is deliberately absent: callers must resolve the acknowledged/uncertain commit
outcome rather than assume that cancelling a future rolls back durable work.

The worker validates stored schema/data and creates/verifies its ordered
`(model,frame,y,x)` retrieval index when reopening. Searches use model/frame equality
and a y range, then recheck x and compute exact vector distances. The standalone
single-frame fixture uses `(model,y,x)`.

The worker checkpoints at startup and orderly shutdown. After 256 successful
mutation requests it marks maintenance due, waits for an empty queue, and defers
at most one second by default before servicing it. `checkpoint_every=0` disables
these periodic worker checkpoints; `try_checkpoint()` inserts an explicit FIFO
maintenance barrier. Core storage can still checkpoint within commit to bound its
log, and an in-progress checkpoint cannot be preempted. Scheduling idle work
reduces contention but does not guarantee a retrieval deadline. Maintenance occurs
on the owner thread, never concurrently with database calls. Shutdown drains
accepted work and joins the thread; I/O can delay it. The database and registry
remain alive until all accepted requests have completed. Applications must
externally coordinate worker destruction with producers.

## Reliability evidence and deployment gate

The worker test covers capacity rejection, durable updates, pruning, separate model
and frame spaces, cancelled reads, reopening and draining accepted writes at
shutdown. `coresql_landmark_soak` adds repeated ingest/prune/search cycles over a
1,000-row fixture, bursts exercising queue backpressure, ten reopenings, and a final
full sequence/count check. It reports process peak RSS and file size over time.
Run it alongside the larger operational size sweep; its small active map is a
repeated-operation test, not a substitute for the 100,000-row memory profile.

Before deploying on a robot, repeat the profiles and fault tests on the target
hardware/filesystem, set application latency and retention requirements, and run
for the intended mission duration. Synthetic tests and process termination do not
establish power-loss behavior of a particular device or perception accuracy.

## Write coalescing and diagnostics

Consecutive ingest requests can share one synchronized commit. Defaults are at
most 16 requests/128 observations and a 500-microsecond collection window measured
from the first request's acceptance. Configurable bounds are 1..64 requests,
32..256 observations and 0..10 ms waiting. `batch_requests=1` disables coalescing.
A search, erase or explicit checkpoint is a FIFO barrier; it cannot be overtaken.
Each ingestion request has its own savepoint: a failure rolls back that request's
observations while neighboring successful requests remain eligible to commit.
Successful futures complete only after their shared commit. A commit failure is
reported to every otherwise successful request in that group.

`stats(after_sequence)` returns counters and the newest 4,096 latency samples,
optionally restricted to sequence numbers newer than the caller's cursor. It
records queue wait, per-request execution, shared commit time and total time from
acceptance to completion. Total time includes work for neighboring batched
requests, so it need not equal the sum of the other fields. Commit time is shared
and must not be summed across requests as independent device work. Idle maintenance
has its own sample (without a request total); explicit checkpoints include request
latency. Gaps in sequence numbers identify overwritten samples. Diagnostics are
bounded, and reading stats requires no database call. They are observations, not
latency guarantees. Preparation/input validation before acceptance is excluded.
