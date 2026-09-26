#Requires -Version 5.1
<#
.SYNOPSIS
    Provision the supported native Windows x64 toolchain: MSYS2 CLANG64.
.DESCRIPTION
    -SkipInstall verifies an existing installation. -Full runs the canonical
    verification entry. No GCC, ARM64 or WSL fallback is selected.
#>
[CmdletBinding()]
param([switch]$Full, [switch]$SkipInstall, [string]$MsysRoot = 'C:\msys64')
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Only native Windows x64 is supported.'
}
$shell = Join-Path $MsysRoot 'msys2_shell.cmd'
if (-not (Test-Path -LiteralPath $shell)) {
    if ($SkipInstall) { throw "MSYS2 is missing at $MsysRoot." }
    if ($MsysRoot -ne 'C:\msys64') { throw 'Install MSYS2 at the requested path before using a custom -MsysRoot.' }
    & winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw 'MSYS2 installation failed.' }
}
if (-not $SkipInstall) {
    & $shell -clang64 -defterm -no-start -c 'pacman -S --needed --noconfirm mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-compiler-rt mingw-w64-clang-x86_64-clang-tools-extra mingw-w64-clang-x86_64-cppcheck mingw-w64-clang-x86_64-zlib mingw-w64-clang-x86_64-python3 mingw-w64-clang-x86_64-ccache make git zip unzip diffutils'
    if ($LASTEXITCODE -ne 0) { throw 'Provisioning failed; complete the MSYS2 system update and retry.' }
}
$clang = Join-Path $MsysRoot 'clang64\bin\clang.exe'
$target = & $clang -dumpmachine
if ($LASTEXITCODE -ne 0 -or $target -notmatch '^x86_64-w64-windows-gnu') { throw "Unexpected compiler target: $target" }
& $clang --version
if ($Full) {
    & "$PSScriptRoot/verify-windows.ps1" -MsysRoot $MsysRoot
} else {
    Write-Host 'Toolchain ready. Verify with: ./scripts/verify-windows.ps1'
    Write-Host 'The full check also requires Node.js and the Go version from pkg/go/go.mod.'
}
