# Serves this project folder over SFTP from a container so another machine can fetch builds / drop files.
#   .\tools\sftp.ps1                 -> start (prints host/port/user/password)
#   .\tools\sftp.ps1 -Stop           -> stop and remove the container
#   .\tools\sftp.ps1 -Port 2222 -User mapx -Password ...   (defaults: 2222, mapx, random)
param(
    [switch]$Stop,
    [int]$Port = 2222,
    [string]$User = "mapx",
    [string]$Password = ""
)
$ErrorActionPreference = "Stop"
$name = "map-expander-sftp"
$root = Split-Path $PSScriptRoot -Parent

if ($Stop) {
    docker rm -f $name 2>$null | Out-Null
    Write-Host "stopped $name"
    return
}

if ($Password -eq "") {
    $chars = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789".ToCharArray()
    $Password = -join (1..16 | ForEach-Object { $chars | Get-Random })
}

docker rm -f $name 2>$null | Out-Null
docker run -d --name $name --restart unless-stopped -p "${Port}:22" `
    -v "${root}:/home/${User}/map-expander" `
    atmoz/sftp "${User}:${Password}:1001" | Out-Null

$ips = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.InterfaceAlias -notlike '*WSL*' -and $_.InterfaceAlias -notlike '*Ethernet 2*' }
Write-Host "SFTP running as container '$name'"
Write-Host "  user:     $User"
Write-Host "  password: $Password"
Write-Host "  port:     $Port"
foreach ($ip in $ips) { Write-Host "  sftp://${User}@$($ip.IPAddress):$Port/map-expander   ($($ip.InterfaceAlias))" }
