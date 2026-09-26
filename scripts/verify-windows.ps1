#Requires -Version 5.1
<#
.SYNOPSIS
    Canonical Windows native x64 verification, shared by developers and CI.
.DESCRIPTION
    Requires MSYS2 CLANG64, Node.js and Go (for package wrapper tests).
    Default runs every phase. -Suites is an explicit native-only iteration.
    ASan/UBSan are enabled by default; -NoSanitizer records a functional-only run.
#>
[CmdletBinding()]
param(
    [ValidateSet('All', 'Native', 'Product', 'Frontend', 'Packages', 'Lint')]
    [string]$Phase = 'All',
    [string]$Suites,
    [string]$MsysRoot = 'C:\msys64',
    [string]$BuildDir = 'build/c',
    [switch]$NoSanitizer
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($env:OS -ne 'Windows_NT' -or $architecture -ne 'X64') {
    throw 'Only native Windows x64 is supported (ARM64/emulation/WSL are unsupported).'
}
if ($Suites -and $Phase -notin @('All', 'Native')) { throw '-Suites requires the Native phase.' }
if ($Suites -and $Suites -notmatch '^[a-zA-Z0-9_, ]+$') { throw 'Invalid suite list.' }
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$clang = Join-Path $MsysRoot 'clang64\bin\clang.exe'
$msys2Command = (Get-Command msys2 -ErrorAction SilentlyContinue).Source
if (-not (Test-Path -LiteralPath $bash) -or -not (Test-Path -LiteralPath $clang)) {
    throw 'MSYS2 CLANG64 is required. Run scripts/setup-windows-toolchain.ps1 first.'
}
$compilerTarget = & $clang -dumpmachine
if ($LASTEXITCODE -ne 0 -or $compilerTarget -notmatch '^x86_64-w64-windows-gnu') {
    throw "Unexpected compiler target: $compilerTarget"
}
$savedEnvironment = @{}
foreach ($key in @('PATH','MSYSTEM','CHERE_INVOKING','TEMP','TMP','TMPDIR','CBM_CI_TEMP_ROOT','CBM_VERIFY_BUILD_DIR')) {
    $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
$previousDirectory = Get-Location
$ownedTemp = $null
$measurements = @()
function Invoke-Check([string]$Name, [string]$Command) {
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    $timer = [Diagnostics.Stopwatch]::StartNew()
    if ($script:msys2Command) {
        & $script:msys2Command -c $Command
    } else {
        & $bash -c "export MSYSTEM=CLANG64; $Command"
    }
    $result = $LASTEXITCODE
    $timer.Stop()
    $script:measurements += [pscustomobject]@{ phase=$Name; seconds=$timer.Elapsed.TotalSeconds; exit=$result; sanitizer=$(if ($Name -eq 'Native') { -not $NoSanitizer } else { $null }) }
    if ($result -ne 0) { throw "$Name failed (exit $result)." }
}
try {
    Set-Location -LiteralPath $repoRoot
    $toolPaths = @('git','node','go','pwsh','python') | ForEach-Object {
        $tool = Get-Command $_ -ErrorAction SilentlyContinue
        if ($tool) { Split-Path -Parent $tool.Source }
    }
    $gitPath = Split-Path -Parent (Get-Command git -ErrorAction Stop).Source
    $env:PATH = (@($gitPath, (Join-Path $MsysRoot 'clang64\bin'), (Join-Path $MsysRoot 'usr\bin')) + $toolPaths + @("$env:SystemRoot\system32", $env:SystemRoot, "$env:SystemRoot\System32\WindowsPowerShell\v1.0") | Select-Object -Unique) -join ';'
    $env:MSYSTEM = 'CLANG64'
    $env:CHERE_INVOKING = '1'
    $env:CBM_VERIFY_BUILD_DIR = $BuildDir
    if (-not $env:CBM_CI_TEMP_ROOT) {
        $ownedTemp = & "$PSScriptRoot/ci/new-protected-temp-root.ps1" -Prefix 'cbm-verify-' -ProtectDir (Join-Path $repoRoot $BuildDir)
        $env:CBM_CI_TEMP_ROOT = $ownedTemp
    }
    $env:TEMP = $env:CBM_CI_TEMP_ROOT
    $env:TMP = $env:TEMP
    $env:TMPDIR = (& $bash -c 'cygpath -u "$CBM_CI_TEMP_ROOT"').Trim()
    if ($NoSanitizer) { Write-Warning 'Functional native run only: ASan/UBSan are disabled explicitly.' }
    Invoke-Check 'Metadata' 'python3 scripts/check-product-metadata.py'
    if ($Suites) { $Phase = 'Native' }
    if ($Phase -in @('All','Native')) {
        $nativeCommandParts = @('scripts/test.sh','CC=clang','CXX=clang++')
        if ($NoSanitizer) { $nativeCommandParts += 'SANITIZE=' }
        if ($Suites) {
            $nativeCommandParts += '--suites'
            $nativeCommandParts += '"' + $Suites + '"'
        }
        $nativeCommandParts += '"BUILD_DIR=$CBM_VERIFY_BUILD_DIR"'
        Invoke-Check 'Native' ($nativeCommandParts -join ' ')
    }
    if ($Phase -in @('All','Frontend')) {
        Invoke-Check 'Frontend' 'cd graph-ui && npm ci && npm run test:coverage && npm run build && npm run test:browser:install && npm run test:browser'
    }
    if ($Phase -in @('All','Packages')) {
        Invoke-Check 'Packages' 'scripts/ci/test-package-wrappers.sh'
    }
    if ($Phase -in @('All','Product')) {
        Invoke-Check 'Product build' 'scripts/build.sh --with-ui CC=clang CXX=clang++ TEST_SEAMS=1 "BUILD_DIR=$CBM_VERIFY_BUILD_DIR"'
        $guardTimer = [Diagnostics.Stopwatch]::StartNew()
        & "$PSScriptRoot/test-windows.ps1" -GuardsOnly -Binary (Join-Path $BuildDir 'memory-for-ai.exe')
        $guardExit = $LASTEXITCODE
        $guardTimer.Stop()
        $measurements += [pscustomobject]@{ phase='Product guards'; seconds=$guardTimer.Elapsed.TotalSeconds; exit=$guardExit; sanitizer=$false }
        if ($guardExit -ne 0) { throw 'Windows product guards failed.' }
    }
    if ($Phase -in @('All','Lint')) {
        Invoke-Check 'Lint' 'scripts/lint.sh --ci CC=clang CXX=clang++ && scripts/ci/lint-mem.sh clang-tidy'
    }
} finally {
    $reportDir = Join-Path $repoRoot 'build'
    if (Test-Path -LiteralPath $reportDir) {
        ConvertTo-Json -InputObject $measurements | Set-Content -LiteralPath (Join-Path $reportDir "windows-x64-$Phase-verification.json") -Encoding UTF8
    }
    foreach ($key in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $savedEnvironment[$key], 'Process') }
    Set-Location -LiteralPath $previousDirectory.Path
    if ($ownedTemp) {
        $resolvedTemp = [IO.Path]::GetFullPath($ownedTemp)
        $profileRoot = [IO.Path]::GetFullPath([Environment]::GetFolderPath('UserProfile')).TrimEnd('\') + '\'
        if ($resolvedTemp.StartsWith($profileRoot, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolvedTemp).StartsWith('cbm-verify-')) {
            Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
        } else { throw "Refusing cleanup outside the verification temp directory: $resolvedTemp" }
    }
}
