[CmdletBinding()]
param(
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$distRoot = Join-Path $repoRoot 'dist'
$stageRoot = Join-Path $distRoot 'OSF Identity'
$zipPath = Join-Path $distRoot 'OSF-Identity-0.1.0.zip'

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

    & xmake f -m releasedbg
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
    $docsDir = Join-Path $stageRoot 'Documentation\OSF Identity'
    $examplesConfigDir = Join-Path $docsDir 'Examples\OSFIdentity'
    [void](New-Item -ItemType Directory -Path $configDir -Force)
    [void](New-Item -ItemType Directory -Path $docsDir -Force)
    [void](New-Item -ItemType Directory -Path $examplesConfigDir -Force)

    Copy-Item -LiteralPath $dll -Destination $pluginDir
    foreach ($schema in @('package.schema.json', 'preset-metadata.schema.json')) {
        $schemaPath = Join-Path $repoRoot "fixtures\osf-identity\$schema"
        Copy-Item -LiteralPath $schemaPath -Destination $configDir
        Copy-Item -LiteralPath $schemaPath -Destination $examplesConfigDir
    }
    $examplePackagesDir = Join-Path $examplesConfigDir 'Packages'
    [void](New-Item -ItemType Directory -Path $examplePackagesDir -Force)
    Copy-Item -LiteralPath (Join-Path $repoRoot 'fixtures\osf-identity\Packages\project.community-example') -Destination $examplePackagesDir -Recurse
    Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $docsDir
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $docsDir
    Copy-Item -LiteralPath (Join-Path $repoRoot 'CHANGELOG.md') -Destination $docsDir
    Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\PLAYER_GUIDE.md') -Destination $docsDir
    Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\AUTHOR_GUIDE.md') -Destination $docsDir
    Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\COMPATIBILITY.md') -Destination $docsDir

    Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    $size = (Get-Item -LiteralPath $zipPath).Length
    Write-Host "[release] $zipPath" -ForegroundColor Green
    Write-Host "[release] bytes=$size sha256=$hash"
}
finally {
    Pop-Location
}
