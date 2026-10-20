param(
    [Parameter(Mandatory = $true)][string]$EditorZip,
    [Parameter(Mandatory = $true)][string]$GameZip,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$editorZip = [IO.Path]::GetFullPath($EditorZip)
$gameZip = [IO.Path]::GetFullPath($GameZip)
$evidenceRoot = [IO.Path]::GetFullPath($EvidenceDirectory)
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$pfxBase64 = $env:AETHER_WINDOWS_SIGNING_PFX_BASE64
$pfxPassword = $env:AETHER_WINDOWS_SIGNING_PFX_PASSWORD
$hasPfx = -not [string]::IsNullOrWhiteSpace($pfxBase64)
$hasPassword = -not [string]::IsNullOrWhiteSpace($pfxPassword)
if ($hasPfx -ne $hasPassword) {
    throw 'Set both AETHER_WINDOWS_SIGNING_PFX_BASE64 and AETHER_WINDOWS_SIGNING_PFX_PASSWORD, or leave both unset.'
}
if (-not (Test-Path -LiteralPath $editorZip -PathType Leaf) -or -not (Test-Path -LiteralPath $gameZip -PathType Leaf)) {
    throw 'Both unsigned Windows release archives must be present.'
}
if (-not (Test-Path -LiteralPath (Join-Path $evidenceRoot 'RELEASE_MANIFEST.json') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $evidenceRoot 'SHA256SUMS.txt') -PathType Leaf)) {
    throw 'Release evidence must contain RELEASE_MANIFEST.json and SHA256SUMS.txt.'
}

if ((Test-Path -LiteralPath $outputRoot) -and (Get-ChildItem -LiteralPath $outputRoot -Force | Select-Object -First 1)) {
    throw 'Release output directory must be empty.'
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$editorOutput = Join-Path $outputRoot 'AetherEditor-windows-x64.zip'
$gameOutput = Join-Path $outputRoot 'AETHER-01-windows-x64.zip'
$manifestOutput = Join-Path $outputRoot 'RELEASE_MANIFEST.json'
$sumsOutput = Join-Path $outputRoot 'SHA256SUMS.txt'
$signed = $false
$signingInfo = [ordered]@{ scheme = 'unsigned'; reason = 'No Windows Authenticode certificate secrets were configured.' }
$certificate = $null
$pfxPath = $null
$imported = @()

try {
    if ($hasPfx) {
        $windowsKits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
        $signTool = Get-ChildItem -LiteralPath $windowsKits -Directory -ErrorAction Stop |
            ForEach-Object {
                $candidate = Join-Path $_.FullName 'x64/signtool.exe'
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { Get-Item -LiteralPath $candidate }
            } |
            Sort-Object { [version]((Split-Path (Split-Path $_.FullName -Parent) -Leaf)) } -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $signTool) { throw 'Windows SDK signtool.exe was not found.' }

        $pfxPath = Join-Path $env:RUNNER_TEMP 'aether-windows-release-signing.pfx'
        [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($pfxBase64))
        $securePassword = ConvertTo-SecureString -String $pfxPassword -AsPlainText -Force
        $imported = @(Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation 'Cert:\CurrentUser\My' -Password $securePassword)
        $certificate = $imported |
            Where-Object { $_.HasPrivateKey -and $_.NotBefore -le (Get-Date) -and $_.NotAfter -gt (Get-Date) } |
            Where-Object { @($_.EnhancedKeyUsageList | ForEach-Object { $_.ObjectId.Value }) -contains '1.3.6.1.5.5.7.3.3' } |
            Select-Object -First 1
        if (-not $certificate) { throw 'The PFX has no currently valid certificate with the code-signing usage.' }

        $workRoot = Join-Path $env:RUNNER_TEMP 'aether-windows-release-signing'
        if (Test-Path -LiteralPath $workRoot) { Remove-Item -LiteralPath $workRoot -Recurse -Force }
        $editorRoot = Join-Path $workRoot 'AetherEditor'
        $gameRoot = Join-Path $workRoot 'AETHER-01'
        New-Item -ItemType Directory -Force -Path $editorRoot, $gameRoot | Out-Null
        Expand-Archive -LiteralPath $editorZip -DestinationPath $editorRoot -Force
        Expand-Archive -LiteralPath $gameZip -DestinationPath $gameRoot -Force

        foreach ($root in @($editorRoot, $gameRoot)) {
            $binaries = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') })
            if ($binaries.Count -eq 0) { throw "No Windows binaries found in $root." }
            foreach ($binary in $binaries) {
                & $signTool sign /sha1 $certificate.Thumbprint /s My /fd SHA256 /tr 'http://timestamp.digicert.com' /td SHA256 /d 'Aether Engine' $binary.FullName
                if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed for $($binary.Name)." }
                & $signTool verify /pa /all $binary.FullName
                if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed for $($binary.Name)." }
            }
        }

        $buildMetadataPath = Join-Path $gameRoot 'BUILD.json'
        if (-not (Test-Path -LiteralPath $buildMetadataPath -PathType Leaf)) { throw 'AETHER-01 BUILD.json is missing.' }
        $buildMetadata = Get-Content -LiteralPath $buildMetadataPath -Raw | ConvertFrom-Json
        $buildMetadata.signed = $true
        [IO.File]::WriteAllText($buildMetadataPath, ($buildMetadata | ConvertTo-Json -Depth 8) + "`n", [Text.UTF8Encoding]::new($false))

        $gameFiles = Get-ChildItem -LiteralPath $gameRoot -Recurse -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object FullName
        $gameSums = foreach ($file in $gameFiles) {
            $relative = $file.FullName.Substring($gameRoot.Length + 1).Replace('\', '/')
            "$( (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $relative"
        }
        [IO.File]::WriteAllText((Join-Path $gameRoot 'SHA256SUMS.txt'), ($gameSums -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
        Compress-Archive -Path (Join-Path $editorRoot '*') -DestinationPath $editorOutput -Force
        Compress-Archive -Path (Join-Path $gameRoot '*') -DestinationPath $gameOutput -Force
        $signed = $true
        $signingInfo = [ordered]@{
            scheme = 'Authenticode'
            certificateSubject = $certificate.Subject
            certificateThumbprint = $certificate.Thumbprint
            timestampAuthority = 'http://timestamp.digicert.com'
        }
    } else {
        Copy-Item -LiteralPath $editorZip -Destination $editorOutput
        Copy-Item -LiteralPath $gameZip -Destination $gameOutput
    }

    $manifest = Get-Content -LiteralPath (Join-Path $evidenceRoot 'RELEASE_MANIFEST.json') -Raw | ConvertFrom-Json
    $manifest.signed = $signed
    $manifest | Add-Member -Force -NotePropertyName signing -NotePropertyValue $signingInfo
    $artifacts = foreach ($path in @($editorOutput, $gameOutput)) {
        $file = Get-Item -LiteralPath $path
        [ordered]@{ name = $file.Name; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $manifest.artifacts = @($artifacts)
    [IO.File]::WriteAllText($manifestOutput, ($manifest | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false))
    $sumLines = foreach ($path in @($editorOutput, $gameOutput)) {
        $file = Get-Item -LiteralPath $path
        "$( (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $($file.Name)"
    }
    [IO.File]::WriteAllText($sumsOutput, ($sumLines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
} finally {
    foreach ($cert in $imported) {
        if ($cert -and $cert.Thumbprint) {
            Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force -ErrorAction SilentlyContinue
        }
    }
    if ($pfxPath -and (Test-Path -LiteralPath $pfxPath)) {
        Remove-Item -LiteralPath $pfxPath -Force -ErrorAction SilentlyContinue
    }
}

Write-Output "Release assets staged in $outputRoot (Authenticode signed: $signed)."
