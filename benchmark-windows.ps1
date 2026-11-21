# 20M-event benchmark. Generates the dataset once, runs the container, and
# optionally compares every row with a reference output (e.g. the accepted V1).
param(
    [string]$Reference = '',          # folder holding user_daily/ and campaign_daily/
    [int]$Events = 20000000,
    [int]$Clicks = 5000000,
    [int]$Seed = 20
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
function Check-Exit([string]$phase) { if ($LASTEXITCODE -ne 0) { throw "$phase failed (exit code $LASTEXITCODE)." } }

docker build --platform linux/amd64 -t shopping-sessions .
Check-Exit 'Image build'
if (!(Test-Path 'bench-data\manifest.json')) {
    docker run --rm --platform linux/amd64 -v "${PWD}\generator:/work" -w /work rust:1.85-slim-bookworm cargo build --release
    Check-Exit 'Generator build'
    New-Item -ItemType Directory -Path bench-data -Force | Out-Null
    docker run --rm --platform linux/amd64 --cpus 8 -v "${PWD}\generator:/work:ro" -v "${PWD}\bench-data:/data" -w /work rust:1.85-slim-bookworm /work/target/release/generate --seed $Seed --events $Events --clicks $Clicks --output /data --threads 8
    Check-Exit 'Data generation'
}
if (Test-Path bench-output) { Remove-Item bench-output -Recurse -Force }
New-Item -ItemType Directory -Path bench-output | Out-Null

$watch = [System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -v "${PWD}\bench-data:/data:ro" -v "${PWD}\bench-output:/output" shopping-sessions
$watch.Stop()
Check-Exit 'Pipeline'
Write-Host ('BENCH_SECONDS={0:N2}' -f $watch.Elapsed.TotalSeconds) -ForegroundColor Green
py scripts/bench-check.py bench-output
Check-Exit 'Invariant check'
if ($Reference -ne '') {
    py scripts/compare.py bench-output $Reference
    Check-Exit 'Row-by-row comparison'
    Write-Host 'MATCH: identical to the reference output.' -ForegroundColor Green
}
