[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Push-Location $repoRoot
try {
    & xmake f -m debug
    if ($LASTEXITCODE -ne 0) { throw "xmake configure failed with exit code $LASTEXITCODE" }

    foreach ($target in @(
        'Runtime NPC Appearance Distributor',
        'npc-appearance-config-tests',
        'npc-appearance-preset-tests'
    )) {
        & xmake build $target
        if ($LASTEXITCODE -ne 0) {
            throw "xmake build failed for '$target' with exit code $LASTEXITCODE"
        }
    }

    $configTests = Join-Path $repoRoot 'build\windows\x64\debug\npc-appearance-config-tests.exe'
    $presetTests = Join-Path $repoRoot 'build\windows\x64\debug\npc-appearance-preset-tests.exe'
    foreach ($test in @($configTests, $presetTests)) {
        if (-not (Test-Path -LiteralPath $test)) { throw "Missing test executable: $test" }
        & $test
        if ($LASTEXITCODE -ne 0) { throw "Test failed with exit code ${LASTEXITCODE}: $test" }
    }

    & python -m unittest tools.tests.npc_appearance_fixture_check_tests
    if ($LASTEXITCODE -ne 0) { throw "Fixture validator tests failed with exit code $LASTEXITCODE" }

    & python .\tools\re\npc_appearance_fixture_check.py
    if ($LASTEXITCODE -ne 0) { throw "Fixture provenance gate failed with exit code $LASTEXITCODE" }

    if (Test-Path -LiteralPath (Join-Path $repoRoot '.git')) {
        & git diff --check
        if ($LASTEXITCODE -ne 0) { throw "git diff --check failed with exit code $LASTEXITCODE" }
    }

    Write-Host '[verify] all standalone checks passed' -ForegroundColor Green
}
finally {
    Pop-Location
}
