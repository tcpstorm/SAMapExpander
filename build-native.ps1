# Builds MapExpander.SA.asi natively on Windows, without Docker. Same toolchain as the Docker image:
# clang (MSYS2, x64 host) cross-compiling to i686-w64-mingw32 against MSYS2's i686 GCC libstdc++.
# premake and plugin-sdk are pinned to the versions in the Dockerfile, so the two builds cannot drift.
#
# One-time host setup:
#   winget install MSYS2.MSYS2
#   C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"   (run twice: the first pass updates the core)
#   C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm make mingw-w64-x86_64-clang mingw-w64-i686-gcc mingw-w64-i686-binutils"
#
#   .\build-native.ps1            -> Release
#   .\build-native.ps1 debug      -> Debug
#   .\build-native.ps1 -Rebuild   -> also re-fetch premake and rebuild plugin-sdk
#
# MSYS2_ROOT (default C:\msys64) and MAPEXPANDER_TOOLCHAIN (default %LOCALAPPDATA%\SAMapExpander-toolchain)
# override the locations.
param(
    [string]$Config = "release",
    [switch]$Rebuild
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$Config = $Config.ToLower()
if ($Config -notin "release", "debug") { throw "config must be release or debug" }

$msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$bash = Join-Path $msys "usr\bin\bash.exe"
if (-not (Test-Path $bash)) { throw "MSYS2 not found at $msys (see the setup notes at the top of this script)" }
foreach ($tool in "mingw64\bin\clang++.exe", "mingw32\bin\g++.exe", "usr\bin\make.exe") {
    if (-not (Test-Path (Join-Path $msys $tool))) { throw "missing $tool in MSYS2 (see the setup notes at the top of this script)" }
}

# Versions come from the Dockerfile so there is one place to bump them.
$dockerfile = Get-Content (Join-Path $root "Dockerfile") -Raw
function Get-DockerArg([string]$name) {
    if ($dockerfile -notmatch "(?m)^ARG $name=(\S+)") { throw "ARG $name not found in Dockerfile" }
    $Matches[1]
}
$premakeVersion = Get-DockerArg "PREMAKE_VERSION"
$sdkCommit = Get-DockerArg "PLUGIN_SDK_COMMIT"

$tc = if ($env:MAPEXPANDER_TOOLCHAIN) { $env:MAPEXPANDER_TOOLCHAIN } else { Join-Path $env:LOCALAPPDATA "SAMapExpander-toolchain" }
$premake = Join-Path $tc "premake-$premakeVersion\premake5.exe"
$sdk = Join-Path $tc "plugin-sdk"
New-Item -ItemType Directory -Force $tc | Out-Null

if ($Rebuild -or -not (Test-Path $premake)) {
    $dir = Split-Path $premake
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $zip = Join-Path $dir "premake.zip"
    Invoke-WebRequest "https://github.com/premake/premake-core/releases/download/v$premakeVersion/premake-$premakeVersion-windows.zip" -OutFile $zip
    Expand-Archive $zip $dir -Force
    Remove-Item $zip
}

if (-not (Test-Path (Join-Path $sdk ".git"))) {
    if (Test-Path $sdk) { Remove-Item -Recurse -Force $sdk }
    git clone -q https://github.com/DK22Pac/plugin-sdk.git $sdk
    if ($LASTEXITCODE -ne 0) { throw "plugin-sdk clone failed" }
}
if ((git -C $sdk rev-parse HEAD) -ne $sdkCommit) {
    git -C $sdk fetch -q origin
    git -C $sdk checkout -q --force $sdkCommit
    if ($LASTEXITCODE -ne 0) { throw "plugin-sdk checkout of $sdkCommit failed" }
    $Rebuild = $true
}

function ConvertTo-MsysPath([string]$p) { $p -replace '\\', '/' }
$env:MSYSTEM = "MINGW32"
$env:CHERE_INVOKING = "1"
$env:PLUGIN_SDK_DIR = ConvertTo-MsysPath $sdk
# The x64 clang targets i686 and takes headers, libstdc++ and CRT from the i686 GCC sysroot. Flags ride
# along in CXX/CC because premake's makefiles use them for both compiling and linking.
$cxx = "/mingw64/bin/clang++ --sysroot=/mingw32"
$cc = "/mingw64/bin/clang --sysroot=/mingw32"
$pm = ConvertTo-MsysPath $premake
$jobs = [Environment]::ProcessorCount

# plugin-sdk archives ~370 objects in one ar call, past Windows' 32K command-line limit for native
# executables. This wrapper hands ar its arguments in a response file instead.
$arWrapper = Join-Path $tc "ar-rsp.sh"
$arScript = @(
    '#!/bin/sh'
    'rsp=$(mktemp)'
    'for a in "$@"; do printf ''"%s"\n'' "$a"; done > "$rsp"'
    'ar "@$rsp"; rc=$?'
    'rm -f "$rsp"'
    'exit $rc'
) -join "`n"
[IO.File]::WriteAllText($arWrapper, "$arScript`n")
$ar = "sh '$(ConvertTo-MsysPath $arWrapper)'"

function Invoke-Msys([string]$dir, [string]$cmd) {
    & $bash -lc "cd '$(ConvertTo-MsysPath $dir)' && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "failed: $cmd" }
}

# Build the plugin-sdk library this config links against, once per pinned commit.
$sdkConfig = $Config
$libName = if ($Config -eq "debug") { "Plugin_d.a" } else { "Plugin.a" }
$sdkLib = Join-Path $sdk "output\lib\$libName"
# plugin-sdk names its archive Plugin.a; the Windows-hosted ld only resolves -lPlugin to libPlugin.a.
$sdkLibAlias = Join-Path $sdk "output\lib\lib$libName"
$stamp = Join-Path $sdk "output\.built-$sdkConfig"
$stampOk = (Test-Path $stamp) -and ((Get-Content $stamp) -eq $sdkCommit) -and (Test-Path $sdkLibAlias)
if ($Rebuild -or -not $stampOk) {
    Invoke-Msys $sdk "'$pm' gmake --file=tools/premake/premake5.lua"
    Invoke-Msys $sdk "make config=$sdkConfig CXX='$cxx' CC='$cc' AR=`"$ar`" -C plugin_sa -j$jobs"
    Copy-Item -Force $sdkLib $sdkLibAlias
    Set-Content $stamp $sdkCommit
}

Invoke-Msys $root "'$pm' gmake --file=premake5.lua"
Invoke-Msys $root "make -C build config=$Config CXX='$cxx' CC='$cc' AR=`"$ar`" -j$jobs"

$out = Join-Path $root ("output\" + (Get-Culture).TextInfo.ToTitleCase($Config) + "\MapExpander.SA.asi")
if (-not (Test-Path $out)) { throw "build finished but $out is missing" }
Get-Item $out | Select-Object FullName, Length, LastWriteTime | Format-List
