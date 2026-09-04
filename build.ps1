# Builds MapExpander.SA.asi inside Docker. Only requirement on the host: Docker Desktop (Linux containers).
#   .\build.ps1            -> Release
#   .\build.ps1 debug      -> Debug
#   .\build.ps1 -Rebuild   -> also rebuild the toolchain image
param(
    [string]$Config = "release",
    [switch]$Rebuild
)
$ErrorActionPreference = "Stop"
$image = "map-expander-toolchain"
$root = $PSScriptRoot

$buildArgs = @("build", "-t", $image, "-f", "$root\Dockerfile", $root)
if ($Rebuild) { $buildArgs += "--no-cache" }
& docker @buildArgs
if ($LASTEXITCODE -ne 0) { throw "toolchain image build failed" }

& docker run --rm -v "${root}:/work" $image $Config.ToLower()
if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }
