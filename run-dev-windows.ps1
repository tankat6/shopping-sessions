# Generates the seed-1 1M-event dataset and compares the output with dev-answer.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
function Check-Exit([string]$phase) { if ($LASTEXITCODE -ne 0) { throw "$phase failed (exit code $LASTEXITCODE)." } }

docker build --platform linux/amd64 -t shopping-sessions .
Check-Exit 'Image build'
docker run --rm --platform linux/amd64 -v "${PWD}\generator:/work" -w /work rust:1.85-slim-bookworm cargo build --release
Check-Exit 'Generator build'
New-Item -ItemType Directory -Path dev-data -Force | Out-Null
docker run --rm --platform linux/amd64 --cpus 8 -v "${PWD}\generator:/work:ro" -v "${PWD}\dev-data:/data" -w /work rust:1.85-slim-bookworm /work/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output /data --threads 8
Check-Exit 'Data generation'
if (Test-Path dev-output) { Remove-Item dev-output -Recurse -Force }
New-Item -ItemType Directory -Path dev-output | Out-Null

$watch = [System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -v "${PWD}\dev-data:/data:ro" -v "${PWD}\dev-output:/output" shopping-sessions
$watch.Stop()
Check-Exit 'Pipeline'
Write-Host ('Container wall time: {0:N2}s' -f $watch.Elapsed.TotalSeconds) -ForegroundColor Yellow
py scripts/compare.py dev-output dev-answer
Check-Exit 'Comparison with dev-answer'
Write-Host 'MATCH: 1M output equals the official answer.' -ForegroundColor Green
