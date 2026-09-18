$ErrorActionPreference = 'Stop'

$packageName = 'memory-for-ai'
$version     = '0.11.0'
$url64       = "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v${version}/memory-for-ai-windows-amd64.zip"
$checksum64  = '579479365d3795b0d5df9221f78e1fb90588ccd89140f9c8b6172355e08098e2'
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
