# Shopping Sessions

Parallel C++ CSV preprocessing and DuckDB session analytics.

## Pipeline

1. Validate CSV records and timestamps.
2. Resolve linked user IDs.
3. Deduplicate and sessionize events.
4. Attribute sessions and count campaign assists.
5. Write Parquet tables to `/output/user_daily/` and `/output/campaign_daily/`.

See `rules.md` for the full specification.

## Build

```sh
docker build --platform linux/amd64 -t shopping-sessions .
```

The image reads `/data`, writes `/output`, and uses `/scratch` for temporary files. It runs without network access and supports the scorer's read-only root filesystem and uid 1000.

## Test on Windows

Requires Docker Desktop and Python with `duckdb` and `polars`.

```powershell
py scripts/run_fixtures.py fixtures -- py pipeline/pipeline.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-dev-windows.ps1
```

The development script builds the image, generates the public 1M-event dataset, and compares both tables with the reference answers.

## Benchmark

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmark-windows.ps1 -Events 20000000 -Clicks 5000000 -Seed 3 -MemoryGB 12 -CPUs 8
```

For repeat runs on an existing dataset:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmark-fast-windows.ps1 -DataPath 'C:\path\to\bench-data'
```

Only successful exact comparisons establish correctness. Accounting checks alone do not.
