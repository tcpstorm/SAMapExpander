# Builds a MapExpander region folder from a GTA III / Vice City install, in a throwaway container.
#   .\tools\import-city.ps1 "D:\Games\GTA Vice City" "D:\Games\GTA San Andreas"
#   .\tools\import-city.ps1 <source> <SA dir> -Lite            (tiles, gxt, water, zones only)
#   .\tools\import-city.ps1 <source> <SA dir> -Region libertycity -NoRename
# Output: <SA dir>\MapExpander\<Region>\ (IMG, col, ide, ipl, tiles, gxt, water, zones, region.ini)
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Region = "vicecity",
    [switch]$Lite,
    [switch]$NoRename
)
$ErrorActionPreference = "Stop"
foreach ($d in $SourceDir, $GameDir) { if (-not (Test-Path $d)) { throw "$d does not exist" } }
$tools = $PSScriptRoot
$flags = "--region=$Region"
if ($Lite) { $flags += " --lite" }
if ($NoRename) { $flags += " --no-rename" }
docker run --rm `
    -v "${SourceDir}:/src:ro" `
    -v "${GameDir}:/sa" `
    -v "${tools}:/tools:ro" `
    python:3-slim sh -c "pip install -q pillow >/dev/null 2>&1; python /tools/import_city.py /src /sa $flags"
