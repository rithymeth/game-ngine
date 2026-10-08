param(
    [string]$BuildDir = 'build',
    [string]$Configuration = 'Release',
    [string]$OutputDir = 'dist/AETHER-01',
    [string]$CookDir = 'build/aether01-cooked',
    [string]$Version = 'prototype'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
$cookRoot = [IO.Path]::GetFullPath((Join-Path $repo $CookDir))
$engineVersionSource = Get-Content -LiteralPath (Join-Path $repo 'engine/include/aether/core/version.h') -Raw
if ($engineVersionSource -notmatch 'kEngineVersion\s*=\s*"([^"]+)"') { throw 'Cannot read the engine release version' }
$releaseNotes = Join-Path $repo ("docs/releases/v" + $matches[1] + '.md')
if (-not (Test-Path -LiteralPath $releaseNotes -PathType Leaf)) { throw "Missing release notes: $releaseNotes" }
if (-not $outputRoot.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package output must be inside the repository workspace'
}
if ((Test-Path -LiteralPath $outputRoot) -and (Get-ChildItem -LiteralPath $outputRoot -Force | Select-Object -First 1)) {
    throw 'Package destination must be empty to exclude stale files'
}
$playerDir = Join-Path $buildRoot "player/$Configuration"
if (-not (Test-Path -LiteralPath (Join-Path $playerDir 'aether_player.exe'))) { $playerDir = Join-Path $buildRoot 'player' }
$player = Join-Path $playerDir 'aether_player.exe'
$pak = Join-Path $cookRoot 'Game.apak'
if (-not (Test-Path -LiteralPath $player) -or -not (Test-Path -LiteralPath $pak)) { throw 'Build the player and cook the game first' }
New-Item -ItemType Directory -Force -Path (Join-Path $outputRoot 'Paks') | Out-Null
Copy-Item -LiteralPath $player -Destination (Join-Path $outputRoot 'AETHER-01.exe')
Get-ChildItem -LiteralPath $playerDir -Filter '*.dll' | Copy-Item -Destination $outputRoot
Copy-Item -LiteralPath $pak -Destination (Join-Path $outputRoot 'Paks/Game.apak')
Copy-Item -LiteralPath (Join-Path $repo 'games/AETHER-01/README.md') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $repo 'games/AETHER-01/THIRD_PARTY_NOTICES.md') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $outputRoot 'ENGINE_NOTICES.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/WINDOWS_REQUIREMENTS.md') -Destination $outputRoot
Copy-Item -LiteralPath $releaseNotes -Destination (Join-Path $outputRoot 'RELEASE_NOTES.md')
Copy-Item -LiteralPath (Join-Path $repo 'third_party/licenses') -Destination (Join-Path $outputRoot 'licenses') -Recurse
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot record the build commit' }
$metadata = [ordered]@{ project = 'AETHER-01'; version = $Version; commit = $commit; configuration = $Configuration; signed = $false }
[IO.File]::WriteAllText((Join-Path $outputRoot 'BUILD.json'), ($metadata | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
$files = Get-ChildItem -LiteralPath $outputRoot -Recurse -File | Sort-Object FullName
$lines = foreach ($file in $files) {
    $relative = $file.FullName.Substring($outputRoot.Length + 1).Replace('\', '/')
    "$( (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $relative"
}
[IO.File]::WriteAllText((Join-Path $outputRoot 'SHA256SUMS.txt'), ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
$zip = Join-Path (Split-Path $outputRoot -Parent) 'AETHER-01-windows-x64.zip'
Compress-Archive -Path (Join-Path $outputRoot '*') -DestinationPath $zip -Force
Write-Output $zip
