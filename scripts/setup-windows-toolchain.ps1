#Requires -Version 5.1
<#
.SYNOPSIS
  One-command Windows toolchain setup + build + test for memory-for-ai.

.DESCRIPTION
  Toolchain resolution order (first match wins):
    1. Repo-bundled GCC at .toolchain\mingw64 (WinLibs GCC 16.x) — fastest,
       no install; SANITIZE= is forced (MinGW GCC has no AddressSanitizer).
    2. MSYS2 CLANG64 (x86-64) / CLANGARM64 (arm64) at C:\msys64 — mirrors the
       CI Windows legs (.github/workflows/_test.yml).
    3. Install MSYS2 via winget, then case 2.

  Default flow: build the production binary (make -f Makefile.cbm cbm), build
  the test runner, then run ONLY the fast 'edit' suite (36 tests for
  edit_symbol / delete_symbol / rename_symbol / undo_edit).

  Switches:
    -Full         run the whole test suite via scripts/test.sh (slow)
    -SkipInstall  toolchain already present; go straight to the build

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup-windows-toolchain.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup-windows-toolchain.ps1 -Full
#>
param(
    [switch]$Full,
    [switch]$SkipInstall
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$MsysRoot = 'C:\msys64'
$BundledBin = Join-Path $RepoRoot '.toolchain\mingw64\bin'

function Write-Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# ── Case 1: repo-bundled WinLibs GCC ─────────────────────────────
if (Test-Path (Join-Path $BundledBin 'gcc.exe')) {
    Write-Step "Using repo-bundled toolchain: $BundledBin"
    & (Join-Path $BundledBin 'gcc.exe') --version | Select-Object -First 1
    $env:PATH = "$BundledBin;$env:PATH"
    $make = Join-Path $BundledBin 'mingw32-make.exe'
    $cc = 'gcc'; $cxx = 'g++'; $san = 'SANITIZE='

    Write-Step 'Building production binary'
    & $make -f Makefile.cbm cbm CC=$cc CXX=$cxx $san
    if ($LASTEXITCODE -ne 0) { throw 'production build failed' }

    Write-Step 'Building test runner'
    & $make -f Makefile.cbm build/c/test-runner CC=$cc CXX=$cxx $san
    if ($LASTEXITCODE -ne 0) { throw 'test-runner build failed' }

    Write-Step "Running suite 'edit'"
    & (Join-Path $RepoRoot 'build\c\test-runner.exe') edit
    if ($LASTEXITCODE -ne 0) { throw "suite 'edit' failed" }

    if ($Full) {
        Write-Step 'Running the FULL test suite (scripts/test.sh — slow)'
        & $make -f Makefile.cbm test CC=$cc CXX=$cxx $san
        if ($LASTEXITCODE -ne 0) { throw 'full test suite failed' }
    }
    Write-Step 'DONE'
    Write-Host 'Production binary: build\c\memory-for-ai.exe' -ForegroundColor Green
    return
}

# ── Case 2/3: MSYS2 CLANG64 / CLANGARM64 (CI-identical) ──────────
$IsArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'
if ($IsArm) { $Msystem = 'CLANGARM64'; $PkgArch = 'aarch64' } else { $Msystem = 'CLANG64'; $PkgArch = 'x86_64' }
$ClangBin = Join-Path $MsysRoot (($Msystem -replace 'CLANG', 'clang') + '\bin\clang.exe')
$ShellFlavor = '-' + $Msystem.ToLower()  # -clang64 / -clangarm64

if (-not $SkipInstall -and -not (Test-Path (Join-Path $MsysRoot 'msys2_shell.cmd'))) {
    Write-Step 'Installing MSYS2 via winget'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw 'winget not found. Install MSYS2 manually from https://www.msys2.org/ then re-run.'
    }
    winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements
    if (-not (Test-Path (Join-Path $MsysRoot 'msys2_shell.cmd'))) {
        throw "MSYS2 install finished but $MsysRoot\msys2_shell.cmd is missing — check the installer output above."
    }
}
if (-not (Test-Path (Join-Path $MsysRoot 'msys2_shell.cmd'))) {
    throw "No toolchain found: $BundledBin\gcc.exe missing and MSYS2 not at $MsysRoot."
}

function Invoke-Msys([string]$Command) {
    & (Join-Path $MsysRoot 'msys2_shell.cmd') $ShellFlavor -defterm -no-start -c $Command
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed ($LASTEXITCODE): $Command" }
}

if (-not $SkipInstall) {
    Write-Step 'pacman system update (two-step, restart-safe)'
    # A core update can kill its own shell mid-run — tolerate failure on the
    # first pass, the second pass completes whatever remains.
    & (Join-Path $MsysRoot 'msys2_shell.cmd') -defterm -no-start -c 'pacman -Syu --noconfirm' 2>$null
    Invoke-Msys 'pacman -Su --noconfirm'

    Write-Step "Installing toolchain packages ($Msystem)"
    $pkgs = @(
        "mingw-w64-clang-$PkgArch-clang",
        "mingw-w64-clang-$PkgArch-compiler-rt",
        "mingw-w64-clang-$PkgArch-zlib",
        "mingw-w64-clang-$PkgArch-python3",
        'make', 'git', 'zip'
    ) -join ' '
    Invoke-Msys "pacman -S --needed --noconfirm $pkgs"
}
if (-not (Test-Path $ClangBin)) { throw "clang not found at $ClangBin — package install did not complete." }
Write-Step "Toolchain OK: $ClangBin"
& $ClangBin --version | Select-Object -First 1

$RepoMsys = '/' + ($RepoRoot -replace '\\', '/' -replace '^([A-Za-z]):', { $_.Groups[1].Value.ToLower() })
Write-Step "Building production binary in $RepoRoot"
Invoke-Msys "cd '$RepoMsys' && make -f Makefile.cbm cbm CC=clang CXX=clang++ SANITIZE="

Write-Step 'Building test runner'
Invoke-Msys "cd '$RepoMsys' && make -f Makefile.cbm build/c/test-runner CC=clang CXX=clang++ SANITIZE="

if ($Full) {
    Write-Step 'Running the FULL test suite (scripts/test.sh — slow)'
    Invoke-Msys "cd '$RepoMsys' && scripts/test.sh CC=clang CXX=clang++"
} else {
    Write-Step "Running suite 'edit' (36 tests)"
    Invoke-Msys "cd '$RepoMsys' && build/c/test-runner edit"
}

Write-Step 'DONE'
Write-Host 'Production binary: build\c\memory-for-ai.exe' -ForegroundColor Green
Write-Host 'Re-run the edit suite anytime:' -ForegroundColor Green
Write-Host "  $MsysRoot\msys2_shell.cmd -defterm -no-start -c `"cd '$RepoMsys' && build/c/test-runner edit`""
