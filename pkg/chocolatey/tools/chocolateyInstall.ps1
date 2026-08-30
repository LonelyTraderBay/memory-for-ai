$ErrorActionPreference = 'Stop'

$packageName = 'memory-for-ai'
$version     = '0.10.8'
$url64       = "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v${version}/memory-for-ai-windows-amd64.zip"
$checksum64  = 'b43ad982994c4d829670749e08d3b622a74bb20041fc0a7d02bef6113f81c34d'
$installDir  = Join-Path $env:ChocolateyBinRoot $packageName

Install-ChocolateyZipPackage `
  -PackageName   $packageName `
  -Url64bit      $url64 `
  -Checksum64    $checksum64 `
  -ChecksumType64 'sha256' `
  -UnzipLocation $installDir

# Shim the binary so it is on PATH
$binPath = Join-Path $installDir 'memory-for-ai.exe'
Install-BinFile -Name 'memory-for-ai' -Path $binPath
