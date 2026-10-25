# Testing

Build with testing enabled, then run from the project directory:

```sh
ctest --test-dir build --output-on-failure
```

CMake registers `unit` and, when Python 3 is available, `recovery`.
Python tests use only the standard library and start real CLI processes.

The unit executable contains 9 groups:

1. Put, overwrite, get copies, erase, empty/binary strings, and restart with a
   different segment count.
2. Invalid options, length limits, and exclusive WAL opening.
3. Random sequential operations against a reference unordered_map.
4. Eight threads with distinct keys, followed by restart and verification of
   every value/deletion.
5. A deliberately paused sync: put is unacknowledged, a reader of the same
   segment waits, and another segment remains readable. After acknowledgement,
   the reader sees the new value.
6. A simulated disk synchronization failure: a batch of eight requests really
   waits for sync, while eight more clients attempt writes. All receive errors,
   and the engine rejects new operations. Recovery may load the already written
   but unacknowledged batch.
7. The last record is cut at every byte position. The preceding record recovers,
   followed by a successful append and another restart.
8. A complete record with a bad CRC, sequence gap, or invalid length is rejected
   without silently truncating the complete record.
9. Basic operations on the standalone memory component.

Python unittest contains 6 tests using separate processes:

- Acknowledged writes, overwrites, and deletions survive a process kill. An
  unacknowledged final write may or may not appear.
- Incomplete prefixes and bodies are truncated before append, and restart works.
- CRC corruption of a complete record is rejected, leaving the file unchanged.
- Duplicate/skipped sequences, unknown operations, and invalid lengths are rejected.
- A second process cannot open the WAL while the first holds it, but can after close.
- The CLI handles spaces/quotes and reports invalid commands.

Tests use bounded waits, CTest timeouts, and a unit watchdog for possible
deadlocks. Failure simulation uses the `before_sync` hook after append and before
sync; this is intentional fault injection, not a claim that the physical disk
was full.

## CLI example

Run the supplied text commands against a new WAL:

```sh
./build/storage_cli --wal demo.wal < examples/commands.txt
```

In PowerShell:

```powershell
Get-Content examples/commands.txt | .\build\storage_cli.exe --wal demo.wal
```

After the example, `name` should be `Mina` and `course` should be absent. Open
the same WAL again to check recovery; the segment count may change on restart.
Generated WAL files are excluded from version control.

## Benchmark

Build in Release mode and run:

```sh
./build/storage_bench --mode all --ops 5000 --keys 1000 --seed 12345 --out benchmark.csv
```

In PowerShell:

```powershell
.\build\storage_bench.exe --mode all --ops 5000 --keys 1000 --seed 12345 --out benchmark.csv
```

The benchmark compares memory and durable modes over 96 configurations with
three repetitions each. The summary CSV stores medians; the raw CSV stores
individual repetitions. Metadata describes the machine and run parameters.
Generated CSVs, metadata, and WAL directories are excluded from version control;
no measurements from a development machine are included in the repository.

See the README for workload details and optional ThreadSanitizer commands.
Benchmark results depend on the machine, workload, and operating system timer
behavior. Short memory/read-only runs require more operations for meaningful
scaling comparisons. Process recovery tests preserve storage and do not simulate
disk failure or power loss.
