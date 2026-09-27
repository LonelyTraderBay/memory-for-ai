# Exercise the actual build-selection function without launching product guards.
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '../../scripts/test-windows.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $scriptPath).Path, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Cannot parse Windows guard runner' }
$definition = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Resolve-Binary'
}, $true)
if (-not $definition) { throw 'Resolve-Binary missing' }
. ([scriptblock]::Create($definition.Extent.Text))

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('cbm-build-selection-' + [guid]::NewGuid())
$repoRoot = $testRoot
$Target = 'cbm-with-ui'
$tmp = $testRoot
$Make = 'Invoke-TestMake'
$script:buildCalls = 0
$script:buildExit = 0
function Invoke-TestMake {
    $script:buildCalls++
    $global:LASTEXITCODE = $script:buildExit
}
try {
    New-Item -ItemType Directory -Path (Join-Path $testRoot 'build/c') -Force | Out-Null
    $binary = Join-Path $testRoot 'build/c/memory-for-ai.exe'
    Set-Content -LiteralPath $binary -Value 'existing artifact'
    $result = Resolve-Binary
    if ($script:buildCalls -ne 1 -or $result -ne $binary) {
        throw 'An existing binary must still go through Make'
    }
    $result = Resolve-Binary -Explicit $binary
    if ($script:buildCalls -ne 1 -or $result -ne $binary) {
        throw '-Binary must select the supplied artifact without building'
    }
    $script:buildExit = 23
    $failed = $false
    try { Resolve-Binary | Out-Null } catch { $failed = $_.Exception.Message -match 'build failed' }
    if (-not $failed) { throw 'A failed build must not fall back to the existing artifact' }
    Write-Output 'PASS: existing binary rebuilt; explicit artifact retained; build failure propagated'
} finally {
    $resolved = [System.IO.Path]::GetFullPath($testRoot)
    $tempParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -or
        [System.IO.Path]::GetFileName($resolved) -notlike 'cbm-build-selection-*') {
        throw 'Refusing cleanup outside the test temporary directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
