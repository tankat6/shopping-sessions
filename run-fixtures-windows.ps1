# Builds the image and runs all 36 fixtures against hand-written expected tables.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)
docker build --platform linux/amd64 -t shopping-sessions .
if ($LASTEXITCODE -ne 0) { throw 'Docker build failed.' }
# run_fixtures.py needs a shell wrapper; use the container directly per fixture.
py scripts/run_fixtures.py fixtures -- py scripts/docker_run_fixture.py
if ($LASTEXITCODE -ne 0) { throw 'Fixtures failed.' }
Write-Host 'All fixtures passed.' -ForegroundColor Green
