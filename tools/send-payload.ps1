# Sends a raw ELF payload to a PS5 ELF loader (e.g. Elf Loader on port 9021) over a plain TCP
# socket - the same thing the SDK's own prospero-deploy does via plink -raw, without needing
# plink.exe on PATH.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$ConsoleHost = '192.168.100.22',
    [int]$Port = 9021
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Path)) { throw "File not found: $Path" }

$bytes = [System.IO.File]::ReadAllBytes($Path)
Write-Host "Sending $Path ($($bytes.Length) bytes) to ${ConsoleHost}:${Port} ..."

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($ConsoleHost, $Port)
$stream = $client.GetStream()
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush()
$stream.Close()
$client.Close()
Write-Host 'Sent.'
