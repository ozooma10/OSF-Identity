[CmdletBinding()]
param(
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$distRoot = Join-Path $repoRoot 'dist'
$stageRoot = Join-Path $distRoot 'OSF Identity'
$zipPath = Join-Path $distRoot 'OSF-Identity-1.0.0.zip'

function Assert-UnderDist([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $dist = [IO.Path]::GetFullPath($distRoot)
    if (-not $full.StartsWith($dist + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify path outside dist: $full"
    }
}

Push-Location $repoRoot
try {
    if (-not $SkipVerify) {
        & (Join-Path $PSScriptRoot 'verify.ps1')
    }

    & xmake f -m releasedbg -y --test_fixtures=n
    if ($LASTEXITCODE -ne 0) { throw "xmake release configure failed with exit code $LASTEXITCODE" }
    & xmake build 'OSF Identity'
    if ($LASTEXITCODE -ne 0) { throw "release build failed with exit code $LASTEXITCODE" }

    $dll = Join-Path $repoRoot 'build\windows\x64\releasedbg\OSF Identity.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw "Release DLL not found: $dll" }

    Assert-UnderDist $stageRoot
    Assert-UnderDist $zipPath
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    $pluginDir = Join-Path $stageRoot 'SFSE\Plugins'
    $configDir = Join-Path $pluginDir 'OSFIdentity'
    [void](New-Item -ItemType Directory -Path $configDir -Force)

    Copy-Item -LiteralPath $dll -Destination $pluginDir
    
    $runtimePacksDir = Join-Path $configDir 'Packs'
    if (Test-Path -LiteralPath $runtimePacksDir) {
        throw "Production staging unexpectedly contains a runtime Packs directory: $runtimePacksDir"
    }

    $bundledPreset = Get-ChildItem -LiteralPath $stageRoot `
        -Recurse `
        -File `
        -Filter '*.npc' |
        Select-Object -First 1

    if ($bundledPreset) {
        throw "Production staging unexpectedly contains an NPC fixture: $($bundledPreset.FullName)"
    }

    Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    $size = (Get-Item -LiteralPath $zipPath).Length
    Write-Host "[release] $zipPath" -ForegroundColor Green
    Write-Host "[release] bytes=$size sha256=$hash"
}
finally {
    Pop-Location
}
