param(
    [long]$Events = 5000000,
    [long]$Clicks = 0,
    [int]$Seed = 2,
    [ValidateRange(2, 24)][int]$MemoryGB = 12,
    [ValidateRange(1, 8)][int]$CPUs = 8
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Clicks -le 0) { $Clicks = [long][math]::Floor($Events / 4) }

function Assert-Exit([string]$Phase) {
    if ($LASTEXITCODE -ne 0) { throw "$Phase failed (exit code $LASTEXITCODE)." }
}
function Create-Clean-Directory([string]$Path) {
    if (Test-Path $Path) { Remove-Item -Path $Path -Recurse -Force }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

Write-Host "Benchmark: $Events events, $Clicks clicks, seed $Seed, $CPUs CPUs, ${MemoryGB}GB memory." -ForegroundColor Cyan
Write-Host '=== 1. Build image from this exact project code ===' -ForegroundColor Cyan
docker build --platform linux/amd64 -t shopping-sessions .
Assert-Exit 'Image build'

Write-Host '=== 2. Build the official generator (if needed) ===' -ForegroundColor Cyan
if (!(Test-Path '.\generator\target\release\generate')) {
    docker run --rm --platform linux/amd64 -v "${PWD}\generator:/work" -w /work rust:1.85-slim-bookworm cargo build --release
    Assert-Exit 'Generator build'
} else {
    Write-Host 'Using already compiled generator.'
}

Write-Host '=== 3. Generate a fresh benchmark dataset (NOT TIMED) ===' -ForegroundColor Cyan
Create-Clean-Directory 'bench-data'
docker run --rm --platform linux/amd64 --cpus $CPUs -v "${PWD}\generator:/work:ro" -v "${PWD}\bench-data:/data" -w /work rust:1.85-slim-bookworm /work/target/release/generate --seed $Seed --events $Events --clicks $Clicks --output /data --threads $CPUs
Assert-Exit 'Data generation'

Write-Host '=== 4. Run pipeline with competition-style limits (TIMED) ===' -ForegroundColor Cyan
Create-Clean-Directory 'bench-output'
Create-Clean-Directory 'bench-scratch'
$timer = [System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus $CPUs --memory "${MemoryGB}g" --memory-swap "${MemoryGB}g" --pids-limit 4096 --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -e NCPU=$CPUs -e MEM_GB=$MemoryGB -v "${PWD}\bench-data:/data:ro" -v "${PWD}\bench-output:/output" -v "${PWD}\bench-scratch:/scratch" shopping-sessions
$timer.Stop()
Assert-Exit 'Pipeline run'
Write-Host ('BENCHMARK_WALL_SECONDS={0:N2}' -f $timer.Elapsed.TotalSeconds) -ForegroundColor Green

Write-Host '=== 5. Validate cross-table accounting invariants (NOT a reference answer) ===' -ForegroundColor Cyan
py .\bench-check.py .\bench-output
Assert-Exit 'Invariant validation'
Write-Host 'Benchmark complete.' -ForegroundColor Green
