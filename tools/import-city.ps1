# Builds a MapExpander region folder from a GTA III / Vice City install, in a throwaway container.
#   .\tools\import-city.ps1 "D:\Games\GTA Vice City" "D:\Games\GTA San Andreas"
#   .\tools\import-city.ps1 <source> <SA dir> -Lite            (tiles, gxt, water, zones only)
#   .\tools\import-city.ps1 <source> <SA dir> -Region libertycity -NoRename
#   .\tools\import-city.ps1 <source> <SA dir> -HudOnly         (just hud\, for [World] HudStyle)
# Output: <SA dir>\MapExpander\<Region>\ (IMG, col, ide, ipl, tiles, gxt, water, zones, region.ini)
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Region,
    [switch]$Lite,
    [switch]$NoRename,
    [switch]$HudOnly
)
$ErrorActionPreference = "Stop"
foreach ($d in $SourceDir, $GameDir) { if (-not (Test-Path $d)) { throw "$d does not exist" } }
$tools = $PSScriptRoot
# -HudOnly with no -Region lets the importer name the folder after the source game, which is
# where the plugin looks; everything else keeps the usual default.
if (-not $Region -and -not $HudOnly) { $Region = "vicecity" }
$flags = if ($Region) { "--region=$Region" } else { "" }
if ($HudOnly) { $flags += " --hud-only" }
if ($Lite) { $flags += " --lite" }
if ($NoRename) { $flags += " --no-rename" }
docker run --rm `
    -v "${SourceDir}:/src:ro" `
    -v "${GameDir}:/sa" `
    -v "${tools}:/tools:ro" `
    python:3-slim sh -c "pip install -q pillow >/dev/null 2>&1; python /tools/import_city.py /src /sa $flags"
