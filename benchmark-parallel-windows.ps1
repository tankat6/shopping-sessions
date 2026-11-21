param(
    [string]$DataPath = "$env:USERPROFILE\Downloads\shopping-sessions-benchmark-ready\starter-duckdb\bench-data",
    [ValidateRange(2,24)][int]$MemoryGB = 12,
    [ValidateRange(1,8)][int]$CPUs = 8
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
& "$PSScriptRoot\benchmark-fast-windows.ps1" -DataPath $DataPath -MemoryGB $MemoryGB -CPUs $CPUs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
