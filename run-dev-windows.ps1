$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Check-Exit([string]$phase) {
    if ($LASTEXITCODE -ne 0) { throw "$phase failed (exit code $LASTEXITCODE)." }
}
function Ensure-Directory([string]$name) {
    New-Item -ItemType Directory -Path $name -Force | Out-Null
}

Write-Host "=== 0. Rebuild the competition image from the UPDATED code ===" -ForegroundColor Cyan
docker build --platform linux/amd64 -t shopping-sessions .
Check-Exit 'Docker image build'

Write-Host "=== 1. Build the official Rust generator in Docker ===" -ForegroundColor Cyan
docker run --rm --platform linux/amd64 -v "${PWD}\generator:/work" -w /work rust:1.85-slim-bookworm cargo build --release
Check-Exit 'Rust generator build'

Write-Host "=== 2. Generate seed-1 public dev dataset: 1M events, 250K clicks ===" -ForegroundColor Cyan
Ensure-Directory 'dev-data'
docker run --rm --platform linux/amd64 --cpus 8 -v "${PWD}\generator:/work:ro" -v "${PWD}\dev-data:/data" -w /work rust:1.85-slim-bookworm /work/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output /data --threads 8
Check-Exit 'Dev data generation'

Write-Host "=== 3. Start a fresh output directory ===" -ForegroundColor Cyan
if (Test-Path 'dev-output') { Remove-Item 'dev-output' -Force -Recurse }
if (Test-Path 'dev-scratch') { Remove-Item 'dev-scratch' -Force -Recurse }
Ensure-Directory 'dev-output'
Ensure-Directory 'dev-scratch'

Write-Host "=== 4. Run the existing competition Docker image ===" -ForegroundColor Cyan
$watch = [System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus 8 --memory 12g --memory-swap 12g --pids-limit 4096 --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -e NCPU=8 -e MEM_GB=12 -v "${PWD}\dev-data:/data:ro" -v "${PWD}\dev-output:/output" -v "${PWD}\dev-scratch:/scratch" shopping-sessions
$watch.Stop()
Check-Exit 'Docker pipeline'
Write-Host ('Docker pipeline wall time: {0:N2} seconds' -f $watch.Elapsed.TotalSeconds) -ForegroundColor Yellow

Write-Host "=== 5. Compare against official dev-answer ===" -ForegroundColor Cyan
py scripts/compare.py dev-output dev-answer
Check-Exit 'Official dev answer comparison'
Write-Host 'SUCCESS: Public 1M-event development tables MATCH exactly.' -ForegroundColor Green
