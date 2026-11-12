# Builds the installer with a stand-in payload and runs it the ways that went wrong for users: a fresh install, an upgrade over a
# running editor, an upgrade while a file is held open (it must fail and leave the old installation intact), and the shortcuts.
# Windows only: it needs the .NET Framework compiler that ships with the OS.
$ErrorActionPreference = 'Stop'
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$source = Join-Path $PSScriptRoot 'AetherInstaller.cs'
$work = Join-Path ([IO.Path]::GetTempPath()) ('aether-installer-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$failures = 0
function Check($ok, $what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures++ }
}
function RunInstaller($exe, $arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru -NoNewWindow
    return $p.ExitCode
}

# The payload: bin\aether_editor_shell.exe is a long-running console program (ping), so an upgrade has something to close.
$stage = Join-Path $work 'stage'
New-Item -ItemType Directory -Path (Join-Path $stage 'bin'), (Join-Path $stage 'share') | Out-Null
Copy-Item "$env:SystemRoot\System32\PING.EXE" (Join-Path $stage 'bin\aether_editor_shell.exe')
Set-Content (Join-Path $stage 'share\readme.txt') 'payload'
$zip = Join-Path $work 'aether-app.zip'
Compress-Archive -Path "$stage\*" -DestinationPath $zip   # Windows PowerShell writes backslash entry names, as the real payload has

(Get-Content $source -Raw).Replace('@VERSION@', '0.0.0-test') | Set-Content (Join-Path $work 'AetherInstaller.cs') -Encoding UTF8
$exe = Join-Path $work 'installer.exe'
& $csc /nologo /target:winexe /optimize+ "/out:$exe" "/resource:$zip,aether-app.zip" /reference:System.IO.Compression.FileSystem.dll /reference:System.IO.Compression.dll /reference:System.Windows.Forms.dll /reference:System.Drawing.dll (Join-Path $work 'AetherInstaller.cs')
Check ($LASTEXITCODE -eq 0 -and (Test-Path $exe)) 'the installer compiles'
if (-not (Test-Path $exe)) { exit 1 }

$target = Join-Path $work 'Aether'
$flags = "/silent /noshortcuts /nolaunch `"/target:$target`""

# 1. A fresh install.
Check ((RunInstaller $exe $flags) -eq 0) 'a fresh install succeeds'
Check (Test-Path (Join-Path $target 'bin\aether_editor_shell.exe')) 'the editor is at bin\aether_editor_shell.exe, not a file with a backslash in its name'
Check (Test-Path (Join-Path $target 'share\readme.txt')) 'the folders from the payload are unpacked as folders'
Check ((Get-Content (Join-Path $target 'INSTALLED_VERSION.txt')).Trim() -eq '0.0.0-test') 'the installed version is recorded'

# 2. An upgrade while the old editor is running from the install folder: the installer closes it and replaces the files.
$running = Start-Process -FilePath (Join-Path $target 'bin\aether_editor_shell.exe') -ArgumentList '-t 127.0.0.1' -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 1
Check (-not $running.HasExited) 'the stand-in editor is running'
Set-Content (Join-Path $target 'old-file.txt') 'from the previous installation'
Check ((RunInstaller $exe $flags) -eq 0) 'an upgrade over a running editor succeeds'
Check ($running.HasExited) 'the running editor was closed'
Check ((Test-Path (Join-Path $target 'bin\aether_editor_shell.exe')) -and -not (Test-Path (Join-Path $target 'old-file.txt'))) 'the old files are gone and the new ones are in'
Check (@(Get-ChildItem -Path $work -Directory -Filter 'Aether.old-*').Count -eq 0) 'no old installation is left beside the new one'

# 3. A file held open by something the installer cannot close. Either the installer cannot move the folder, fails and leaves the old
# installation exactly as it was, or it can and the new installation is complete: never a half-replaced folder.
$held = [IO.File]::Open((Join-Path $target 'share\readme.txt'), 'Open', 'Read', 'None')
try {
    Set-Content (Join-Path $target 'marker.txt') 'old'
    $code = RunInstaller $exe $flags
} finally { $held.Dispose() }
$editorThere = Test-Path (Join-Path $target 'bin\aether_editor_shell.exe')
$markerThere = Test-Path (Join-Path $target 'marker.txt')
Check ((($code -eq 1) -and $markerThere -and $editorThere) -or (($code -eq 0) -and -not $markerThere -and $editorThere)) "with a file held open the install either fails with the old installation intact or completes (exit $code)"
Check ((RunInstaller $exe $flags) -eq 0) 'once the file is released the install succeeds'

# 4. The shortcuts point at the installed editor (and are replaced, not duplicated, on a second run).
$noShortcutFlags = "/silent /nolaunch `"/target:$target`""
Check ((RunInstaller $exe $noShortcutFlags) -eq 0) 'an install with shortcuts succeeds'
Check ((RunInstaller $exe $noShortcutFlags) -eq 0) 'installing again replaces the shortcuts'
$link = Join-Path ([Environment]::GetFolderPath('Programs')) 'Aether\Aether Editor.lnk'
Check (Test-Path $link) 'the Start menu shortcut exists'
if (Test-Path $link) {
    $shell = New-Object -ComObject WScript.Shell
    Check ($shell.CreateShortcut($link).TargetPath -eq (Join-Path $target 'bin\aether_editor_shell.exe')) 'the Start menu shortcut starts the installed editor'
}
Remove-Item $link -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Aether Editor.lnk') -ErrorAction SilentlyContinue

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failures -gt 0) { Write-Host "$failures installer check(s) failed"; exit 1 }
Write-Host 'installer checks passed'
