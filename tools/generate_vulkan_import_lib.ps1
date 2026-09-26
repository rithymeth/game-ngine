# Generates a vulkan-1.lib import library from the Vulkan loader DLL a GPU
# driver already ships (C:\Windows\System32\vulkan-1.dll), for building
# against the Vulkan API on a machine that has a Vulkan-capable driver but
# not the full LunarG Vulkan SDK installed. Only used as a CMake configure-
# time fallback when find_package(Vulkan) can't find a real SDK - see the
# root CMakeLists.txt for when this runs.
#
# The technique: dump the DLL's exported symbol names (dumpbin /EXPORTS),
# turn that into a linker .def file, then have MSVC's lib.exe build an
# import library from the .def + .dll pair. This works because an import
# library only needs to know symbol *names*, not their implementations -
# the actual code stays in the DLL, loaded at runtime as normal.
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [Parameter(Mandatory = $true)][string]$DumpbinPath,
    [Parameter(Mandatory = $true)][string]$LibPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $DllPath)) {
    Write-Error "Vulkan loader DLL not found at $DllPath"
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$dumpFile = Join-Path $OutputDir "vulkan-1.dump.txt"
$defFile = Join-Path $OutputDir "vulkan-1.def"
$libFile = Join-Path $OutputDir "vulkan-1.lib"

& $DumpbinPath /EXPORTS $DllPath | Out-File -FilePath $dumpFile -Encoding ascii

$exportNames = Get-Content $dumpFile | Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(vk\w+)' } |
    ForEach-Object { $matches[1] }

if ($exportNames.Count -eq 0) {
    Write-Error "No vk* exports found in $DllPath - dumpbin output format may have changed"
    exit 1
}

$defLines = @("LIBRARY vulkan-1", "EXPORTS") + $exportNames
Set-Content -Path $defFile -Value $defLines -Encoding ascii

& $LibPath "/DEF:$defFile" "/OUT:$libFile" "/MACHINE:X64" | Out-Null

if (-not (Test-Path $libFile)) {
    Write-Error "lib.exe did not produce $libFile"
    exit 1
}

Write-Host "Generated $libFile from $($exportNames.Count) exports in $DllPath"
exit 0
