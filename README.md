# Multithreaded Storage Engine

A small C++17 key-value engine: the map lives in RAM, and an on-disk WAL supports
recovery. The map is divided into segments with separate `shared_mutex` locks.
One background thread writes groups of changes and acknowledges them only after
disk synchronization. There is no networking, transaction support, cache layer,
or disk index.

## Build and run

Linux, CMake 3.20+, and GCC/Clang:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/storage_cli --wal data.wal --segments 16
```

There are no external C++ libraries. Python 3 with only its standard library is
used for process-crash tests. Add `-DBUILD_TESTING=OFF` to build without tests.

On Windows with MinGW, put `g++`, `mingw32-make`, `cmake`, and `python` in
PATH, then run from the project directory:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
.\build\storage_cli.exe --wal data.wal --segments 16
```

The CLI reads commands from standard input:

```text
put "name" "Ana"
get "name"
put "name" "Mina"
erase "name"
get "name"
quit
```

Responses are OK, VALUE "Ana", OK, ERASED, NOT_FOUND, and BYE, respectively.
Restarting with the same WAL preserves the deletion. `--wal` is required; the
parent directory must already exist. After recovery, the CLI prints READY, the
number of loaded records, and the number of bytes discarded from an incomplete
tail.

An input-file example is available in `examples/commands.txt`:

```sh
./build/storage_cli --wal demo.wal < examples/commands.txt
```

PowerShell: `Get-Content examples/commands.txt | .\build\storage_cli.exe --wal demo.wal`.
After that example, `name=Mina` remains and `course` is deleted.

## Library

```cpp
#include "engine.h"

EngineOptions options;
options.segments = 16;
StorageEngine engine("data.wal", options);
engine.put("name", "Ana");
auto value = engine.get("name");
bool existed = engine.erase("name");
engine.close();
```

`get` returns a copied value in `optional<string>`. The API may be called from
multiple client threads. Call `close` from an external thread once clients have
finished. Calls after shutdown throw; repeating a successful close is allowed.
The destructor also closes the engine, but use explicit close to report shutdown
errors.

The segment count is 1..1024 (1 and 16 in the benchmark). Keys may contain up to
256 bytes and values up to 4096 bytes. The API accepts empty strings and binary
bytes. The CLI is a simple text interface using `std::quoted`, without a special
binary-data language. Failed validation adds no WAL record.

`get` returns a copy under one segment's shared lock. `put` and `erase` hold that
segment's exclusive lock, enqueue a request, and wait on its future. They change
the map and return only after the WAL acknowledgement. Erasing a missing key
returns false without a new record because nothing changed.

## Locks and the WAL thread

Lock order is **segment -> briefly queue_mutex**. Waiting for the future does
not hold queue_mutex. The WAL thread takes only queue_mutex and never a segment
lock. A reader of the same segment therefore waits for a write, while operations
on other segments can submit requests. A segment is selected by
`hash(key) % count` during a run. The segment count may change on restart because
the map is rebuilt.

Sequence numbers are assigned under the queue lock together with enqueueing.
The WAL thread collects up to 64 requests, waiting up to 1000 microseconds from
the start of group formation. These are `EngineOptions::batch_size` and
`batch_wait`. On Windows, the actual duration of a short wait also depends on
system timer resolution.

Each request has a promise. The WAL thread first writes the entire group, then
performs one disk synchronization, and only then fulfills the promises. Linux
uses `fdatasync` and Windows uses `_commit`; writing into an OS buffer alone does
not acknowledge a durable write.
[Linux fsync/fdatasync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html)
and [Microsoft _commit documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/commit).

If append or sync throws, the active batch and queued requests receive an error.
The engine stops accepting get/put/erase. The background thread exits; close joins
it, closes the descriptor, and reports the error again. Waiters are not left on
forgotten promises. If updating the map fails to allocate after the WAL has been
acknowledged, the engine also requires a restart so the map cannot remain stale.

`before_sync` is an empty test hook in the options. Tests use it to pause the WAL
thread at a known point or simulate a synchronization failure. It runs on the
WAL thread after write and before sync; the hook must not call the same engine's
API.

## Format and recovery

Each record uses little-endian numbers. No C++ structure with padding is dumped.

| Field | Bytes |
|---|---:|
| Length of the remaining record | 4 |
| Sequence number, starting at 1 | 8 |
| Operation: PUT=1, ERASE=2 | 1 |
| Key length | 4 |
| Value length | 4 |
| Key and value | Variable |
| CRC32 of preceding body fields, starting with sequence | 4 |

ERASE has no value. Body length is 21..4373 bytes. CRC32 uses polynomial
0xedb88320; the length prefix is not part of the checksum. Sequence numbers must
be consecutive, including after restart.

On opening, the WAL is read from the beginning. Complete records are checked
and replayed into the map. An incomplete final prefix or body is discarded by
truncating to the last valid boundary, with the truncation synchronized before
another append. A complete record with a bad checksum, operation, lengths, or
sequence stops recovery and is not silently repaired. Otherwise the WAL is
retained from the beginning, without compaction.

File is a RAII descriptor wrapper. Read/write loops handle short I/O and EINTR.
Only construction performs replay; afterward only the WAL thread uses the file
until shutdown. A WAL may have only one open engine: Linux uses flock and Windows
uses a share-deny open. On Linux, the parent directory is also synchronized when
opening the WAL.

Acknowledged writes survive restart with the disk preserved. A write recorded
immediately before a crash may appear even if its caller received no response.
A missing response therefore does not mean the operation was rolled back. Tests
simulate process termination, not disk loss or failure.

## Benchmark

```sh
./build/storage_bench --mode all --ops 5000 --keys 1000 --seed 12345 --out benchmark.csv
```

The program uses 1/2/4/8 threads, 1/16 segments, 0/5/50% writes, and uniform/hot
traffic. Hot selection sends approximately 80% of operations to 10 keys and the
remainder to other keys. Each configuration has three repetitions; output
contains the median duration and operations per second. Each repetition gets a
new store and, in durable mode, its own WAL.

Operations are generated in advance using the same seed. For a given percentage
and distribution, the same sequence is used for all thread/segment counts and
repetitions. Threads process fixed portions of that sequence; interleaving
between them depends on the OS. The benchmark uses get and put on existing keys.

Generation, pool startup, initial filling, and recovery are outside the measured
interval. For durable mode, the initial dataset is prepared in the same WAL
format, synchronized, and loaded before the start barrier. Final join and close
are also outside timing. Within the measured interval, each put goes through the
real engine and waits for disk acknowledgement.

`--mode memory` measures only the segmented memory component; `--mode durable`
measures the complete engine. Results have a separate mode column. `--mode all`
produces 96 median rows (48 per mode) and 288 individual repetition rows.

Outputs are `benchmark.csv`, `benchmark.csv.raw.csv`, and `benchmark.csv.meta.txt`.
Metadata includes build, compiler, operation count, seed, logical thread count,
and batching parameters. Exact CPU, physical core count, and disk labels can be
provided with `--cpu-label`, `--physical-cores`, and `--disk-label`. WAL files
remain in a new subdirectory under `bench-data`, recorded in the metadata file.
The output CSV is overwritten when running again with the same `--out`.

## Tests and files

- `table.*`: segmented memory map.
- `file.*`, `wal.*`: descriptor, record format, durability, and replay.
- `engine.*`: synchronous API, queue, and background thread.
- `thread_pool.*`: small benchmark pool copied from project 1, without a runtime
  dependency on that project.
- `main.cpp`: CLI; `benchmark.cpp`: measurements.
- `tests/tests.cpp`: small CHECK runner, concurrency, and WAL edge cases.
- `tests/recovery.py`: Python unittest with real processes, killing, and restart.

Optional Linux TSAN check:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Test coverage and commands are described in `TESTING.md`. Run the benchmark
above to generate measurements for your own machine; generated measurements,
metadata, and WAL files are excluded from version control. The engine has no
snapshots, transactions, WAL rotation, or lock-free structures.
