#Requires -Version 5.1
# Compatibility entry. Source builds use CLANG64, never WSL/CGO.
[CmdletBinding()]
param([switch]$FromSource)
$ErrorActionPreference = 'Stop'
if ($FromSource) {
    & "$PSScriptRoot/setup-windows-toolchain.ps1" -Full
} else {
    & (Join-Path (Split-Path -Parent $PSScriptRoot) 'install.ps1') @args
}
