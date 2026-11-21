param(
    [string]$DataPath = "$env:USERPROFILE\Downloads\shopping-sessions-benchmark-ready\starter-duckdb\bench-data",
    [ValidateRange(2,24)][int]$MemoryGB=12,
    [ValidateRange(1,8)][int]$CPUs=8
)
$ErrorActionPreference='Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DataPath = [System.IO.Path]::GetFullPath($DataPath)
if (!(Test-Path (Join-Path $DataPath 'manifest.json'))) {
    throw "Benchmark data not found at $DataPath. Supply the existing bench-data directory with -DataPath."
}
function Check-Exit([string]$name) {
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit code $LASTEXITCODE)" }
}
foreach ($name in @('profile-output','profile-scratch')) {
    if (Test-Path $name) { Remove-Item $name -Force -Recurse }
    New-Item -ItemType Directory $name | Out-Null
}
Write-Host '=== Build profiled pipeline image ===' -ForegroundColor Cyan
docker build --platform linux/amd64 -t shopping-sessions-profiled .
Check-Exit 'Build'
Write-Host '=== Profile existing 5M benchmark dataset ===' -ForegroundColor Cyan
$watch=[System.Diagnostics.Stopwatch]::StartNew()
docker run --rm --platform linux/amd64 --network none --read-only --tmpfs /tmp:rw,size=64m --cpus $CPUs --memory "${MemoryGB}g" --memory-swap "${MemoryGB}g" --pids-limit 4096 --user 1000:1000 -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch -e NCPU=$CPUs -e MEM_GB=$MemoryGB -e PROFILE_SQL=1 -v "${DataPath}:/data:ro" -v "${PWD}\profile-output:/output" -v "${PWD}\profile-scratch:/scratch" shopping-sessions-profiled
$watch.Stop()
if ($LASTEXITCODE -ne 0) { throw 'Profile pipeline failed' }
Write-Host ('PROFILE_WALL_SECONDS={0:N2}' -f $watch.Elapsed.TotalSeconds) -ForegroundColor Green
Write-Host '=== Cross-table checks ===' -ForegroundColor Cyan
py .\bench-check.py .\profile-output
Check-Exit 'Checks'
Write-Host 'Profiling complete.' -ForegroundColor Green
