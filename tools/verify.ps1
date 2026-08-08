[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Push-Location $repoRoot
try {
    # -y installs missing xmake packages without prompting, so the same script
    # drives an interactive shell and unattended CI identically.
    & xmake f -m debug -y
    if ($LASTEXITCODE -ne 0) { throw "xmake configure failed with exit code $LASTEXITCODE" }

    foreach ($target in @(
        'OSF Identity',
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

    & python -m unittest `
        tools.tests.npc_appearance_fixture_check_tests `
        tools.tests.npc_appearance_schema_tests
    if ($LASTEXITCODE -ne 0) { throw "Python validation tests failed with exit code $LASTEXITCODE" }

    & python .\tools\re\npc_appearance_fixture_check.py
    if ($LASTEXITCODE -ne 0) { throw "Fixture provenance gate failed with exit code $LASTEXITCODE" }

    $sourceFiles = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.h') }
    $deletedLifecycleSymbols = @(
        'RequestNpcAppearanceNativeFrame',
        'OnNpcAppearanceNativeFrame',
        'PendingSceneApply',
        'ObjectLoadedSink',
        'ReferenceSet3dSink',
        'ReferenceDetachSink',
        'SuppressNextSceneSet3d',
        'TargetHoldState',
        'RunTargetRestore',
        'PersistentAppliedState',
        'g_persistentAppliedRefs',
        'RemovePersistentAppearances'
    )
    foreach ($symbol in $deletedLifecycleSymbols) {
        $match = $sourceFiles |
            Select-String -SimpleMatch -Pattern $symbol |
            Select-Object -First 1
        if ($match) {
            throw "Deleted lifecycle symbol '$symbol' returned at $($match.Path):$($match.LineNumber)"
        }
    }
    Write-Host '[verify] deleted lifecycle symbols remain absent' -ForegroundColor Green

    $deletedTargetingSymbols = @(
        'EditorIDTarget',
        'PluginLocalFormIDTarget',
        'AsEditorID',
        'AsPluginLocalFormID',
        'kEditorIDFilenameConvention',
        'kEditorIDFilename',
        'target_editorid_mismatch',
        'LookupByEditorID<RE::TESNPC>'
    )
    foreach ($symbol in $deletedTargetingSymbols) {
        $match = $sourceFiles |
            Select-String -SimpleMatch -Pattern $symbol |
            Select-Object -First 1
        if ($match) {
            throw "Deleted targeting symbol '$symbol' returned at $($match.Path):$($match.LineNumber)"
        }
    }
    Write-Host '[verify] EditorID targeting symbols remain absent' -ForegroundColor Green

    $resolverSourcePath = Join-Path $repoRoot 'src\NpcAppearance\Resolver.cpp'
    $resolverTargetLookup = Select-String -LiteralPath $resolverSourcePath -SimpleMatch -Pattern 'GetFormEditorID'
    if ($resolverTargetLookup) {
        throw "Preset NPCFormEditorID returned to target equality at ${resolverSourcePath}:$($resolverTargetLookup[0].LineNumber)"
    }
    Write-Host '[verify] preset NPCFormEditorID is not used for target equality' -ForegroundColor Green

    if (Test-Path -LiteralPath (Join-Path $repoRoot '.git')) {
        & git diff --check
        if ($LASTEXITCODE -ne 0) { throw "git diff --check failed with exit code $LASTEXITCODE" }
    }

    Write-Host '[verify] all OSF Identity checks passed' -ForegroundColor Green
}
finally {
    Pop-Location
}
