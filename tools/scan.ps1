# Scans gta_sa.exe for compressed-vector usage sites. Runs in a throwaway Python container.
#   .\tools\scan.ps1 "C:\Games\GTA San Andreas"
param([Parameter(Mandatory = $true)][string]$GameDir)
$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
if (-not (Test-Path (Join-Path $GameDir "gta_sa.exe"))) { throw "gta_sa.exe not found in $GameDir" }
docker run --rm -v "${GameDir}:/game:ro" -v "${tools}:/tools:ro" python:3-slim `
    sh -c "pip install -q capstone >/dev/null && python /tools/scan_exe.py /game/gta_sa.exe"
