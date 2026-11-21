param(
    [string]$DataPath = "$env:USERPROFILE\Downloads\shopping-sessions-benchmark-ready\starter-duckdb\bench-data",
    [ValidateRange(2,24)][int]$MemoryGB = 12,
    [ValidateRange(1,8)][int]$CPUs = 8
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DataPath = [System.IO.Path]::GetFullPath($DataPath)
if (!(Test-Path (Join-Path $DataPath 'manifest.json'))) {
    throw "Existing benchmark data not found: $DataPath. Use -DataPath 'C:\path\to\bench-data'"
}
function Check-Exit([string]$stage) {
    if ($LASTEXITCODE -ne 0) { throw "$stage failed (exit code $LASTEXITCODE)" }
}
Write-Host '=== 1. Build FAST pipeline Docker image ===' -ForegroundColor Cyan
docker build --platform linux/amd64 -t shopping-sessions-fast .
Check-Exit 'Docker build'
foreach ($dir in @('fast-output','fast-scratch')) {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir | Out-Null
}
Write-Host '=== 2. TIME the pipeline on your previous 5M dataset ===' -ForegroundColor Cyan
$timer = [System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus $CPUs --memory "${MemoryGB}g" --memory-swap "${MemoryGB}g" --pids-limit 4096 --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -e NCPU=$CPUs -e MEM_GB=$MemoryGB -e PROFILE_SQL=1 -v "${DataPath}:/data:ro" -v "${PWD}\fast-output:/output" -v "${PWD}\fast-scratch:/scratch" shopping-sessions-fast
$timer.Stop()
Check-Exit 'Fast pipeline'
Write-Host ('FAST_BENCHMARK_WALL_SECONDS={0:N2}' -f $timer.Elapsed.TotalSeconds) -ForegroundColor Green
Write-Host '=== 3. Check output totals ===' -ForegroundColor Cyan
py .\bench-check.py .\fast-output
Check-Exit 'Invariant check'
Write-Host 'DONE. Send FAST_BENCHMARK_WALL_SECONDS and INVARIANTS results.' -ForegroundColor Green
