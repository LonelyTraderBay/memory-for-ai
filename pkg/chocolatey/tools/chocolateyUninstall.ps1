$ErrorActionPreference = 'Stop'

$packageName = 'memory-for-ai'
$installDir  = Join-Path $env:ChocolateyBinRoot $packageName

Uninstall-BinFile -Name 'memory-for-ai'

if (Test-Path $installDir) {
  Remove-Item $installDir -Recurse -Force
}
