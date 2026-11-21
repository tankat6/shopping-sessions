# Shopping Sessions: native pipeline (v3)

A single static C++ binary reads the CSV inputs and writes the two Parquet
tables directly. There is no Python, no DuckDB and no intermediate CSV.

## Build and run

```sh
docker build --platform linux/amd64 -t shopping-sessions .
```

The image reads `/data`, writes `/output/user_daily` and
`/output/campaign_daily`, and does not need `/scratch`. It runs as any uid on
a read-only root filesystem. Environment: `DATA_DIR`, `OUTPUT_DIR`, `NCPU`.

## Verify (Linux / macOS)

```sh
make fixtures                         # all 36 fixtures through the container
make dev && make run-dev && make check-dev   # 1M reference comparison
```

## Verify (Windows, Docker Desktop, Python with polars)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-fixtures-windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-dev-windows.ps1
```

## 20M benchmark

```powershell
# generates bench-data once, runs the container, checks invariants
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmark-windows.ps1
# also compare every row with an accepted output (V1 etc.)
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmark-windows.ps1 -Reference C:\path\to\bench-output
```

## Profiling

The binary prints stage timings and row counts to stderr (`QUIET=1` hides
them):

```
STAGE refs                      parse manifest, products, campaigns
STAGE link_parse                parse links
STAGE link_dict                 dictionary, union-find, person names
STAGE parse                     parse events + clicks, shuffle by event id
STAGE dedup                     dedup events, resolve persons, shuffle by person
STAGE sessions+user_daily       sessions, attribution, assists, user_daily files
STAGE campaign_daily            merge and write campaign_daily
```

Local, no container: `scripts/bench_native.sh DATASET` builds the binary and
prints the stages plus wall time, CPU time and peak RSS. Use `NCPU=n` to test
thread scaling.

## Extra correctness tests

```sh
scripts/diff_test.sh BINARY DATASET REFERENCE_OUTPUT   # threads, chunk sizes, partitions
python3 scripts/make_long_ids.py DATASET OUT_DATASET   # IDs longer than 15 bytes
g++ -O2 -std=c++20 -mavx2 -mbmi2 -mpopcnt -pthread -DWEAK_HASH src/main.cpp -o weak
```

`CHUNK_BYTES` and `PART_BITS` are test knobs that force chunk-boundary and
partition edge cases. `-DWEAK_HASH` collapses every hash to 8 values to prove
that equality is always exact.

## Design

1. **Links** are parsed in parallel into 16-byte exact keys (IDs up to 15
   bytes are stored inline, longer ones are interned). A partitioned
   read-only dictionary is built without locks, followed by union-find and the
   byte-wise smallest name per person.
2. **Parse** splits each file into 4 MB chunks handled by any thread. A
   SIMD scan finds commas and quotes; timestamps, prices and types are
   validated per the rules. Events are shuffled by `hash(event_id)`.
3. **Dedup** runs per partition with a small open-addressing table
   (earliest `ingest_ts` wins, validation after dedup). Survivors are resolved
   to a person and shuffled by `hash(person)`.
4. **Persons** are processed per partition: counting sort by person, sort by
   `ts`, sessionize, credit the latest click, and compute assists as per-campaign
   unions of session-index ranges (no per-session scan of active campaigns).
   `user_daily` is written straight to per-thread Parquet files.
5. **campaign_daily** is merged from dense per-thread arrays and written once.

Memory is about 80 bytes per input event (roughly 5 GB at 60M events).
