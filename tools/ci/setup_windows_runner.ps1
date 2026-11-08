# Registers this machine as a self-hosted GitHub Actions runner for the Windows CI jobs.
# Usage (elevated PowerShell), token from Repo > Settings > Actions > Runners > New self-hosted runner:
#   .\setup_windows_runner.ps1 -Repo owner/name -Token <registration token>
# Then set the repository variable WINDOWS_RUNNER to ["self-hosted","windows"]. Delete the variable to go back to hosted runners.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Token, [string]$Dir = "C:\actions-runner")
$ErrorActionPreference = "Stop"
$rel = Invoke-RestMethod https://api.github.com/repos/actions/runner/releases/latest
$asset = $rel.assets | Where-Object name -like "actions-runner-win-x64-*.zip" | Select-Object -First 1
New-Item -ItemType Directory -Force $Dir | Out-Null
Invoke-WebRequest $asset.browser_download_url -OutFile "$Dir\runner.zip"
Expand-Archive "$Dir\runner.zip" $Dir -Force
Push-Location $Dir
.\config.cmd --url "https://github.com/$Repo" --token $Token --labels windows --unattended --replace --runasservice
Pop-Location
