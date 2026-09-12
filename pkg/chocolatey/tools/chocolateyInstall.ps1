$ErrorActionPreference = 'Stop'

$packageName = 'memory-for-ai'
$version     = '0.10.9'
$url64       = "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v${version}/memory-for-ai-windows-amd64.zip"
$checksum64  = 'b1c1d1bab8bca8712fff7f7d51b4b9eb37323e44b87317d965eabc2328e37bc8'
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
