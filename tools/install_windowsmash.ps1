<#
.SYNOPSIS
WindowSmash moved to OpenSmash-Ultimate: this installer hands a WindowSmash folder over to it.

.DESCRIPTION
WindowSmash is developed in https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate now. A WindowSmash
folder's installer ("Install WindowSmash.bat") updates from this repository, which no longer changes, so it
brings this file, which:

1. takes OpenSmash-Ultimate's installer (of its latest release, or of main while it has none), checked to be
   that repository's, in place of this one;
2. marks the folder as 8025b48, the last commit both repositories share (this repository's later commits only
   change the README), so that installer can update it;
3. runs it with this run's arguments (without -NoUpdate, which the old installer adds when it restarts).

That installer then updates the folder to OpenSmash-Ultimate and goes on as on any other copy: your game
files, settings, save and builds stay.
#>
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Repo = 'OpenSmash-Ultimate/OpenSmash-Ultimate'
$Shared = '8025b483b0443adb5668395eca31f9250e8c8efb 2026-10-22'
$Headers = @{ 'User-Agent' = 'WindowSmash-installer' }
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

Write-Host 'WindowSmash moved to OpenSmash-Ultimate: handing this folder over to it' -ForegroundColor Cyan
Write-Host "  $Root"
$ref = 'main'
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing -Headers $Headers
    if ($rel.tag_name) { $ref = [string]$rel.tag_name }
} catch { }   # (no release yet: main)

$script = Join-Path $PSScriptRoot 'install_windowsmash.ps1'
$tmp = "$script.new"
try {
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try { Invoke-WebRequest -Uri "https://raw.githubusercontent.com/$Repo/$([uri]::EscapeDataString($ref))/tools/install_windowsmash.ps1" -OutFile $tmp -UseBasicParsing -Headers $Headers }
    finally { $ProgressPreference = $old }
    $text = [IO.File]::ReadAllText($tmp)
    if ($text -notmatch "\`$RepoName = 'OpenSmash-Ultimate'" -or $text -notmatch '\[switch\]\s*\$UpdateOnly\b') {
        throw "the installer GitHub sent is not OpenSmash-Ultimate's"
    }
} catch {
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    Write-Host ''
    Write-Host "ERROR: could not get OpenSmash-Ultimate's installer ($($_.Exception.Message)); nothing was changed. Run this again." -ForegroundColor Red
    exit 1
}
Move-Item -LiteralPath $tmp -Destination $script -Force
[IO.File]::WriteAllText((Join-Path $Root 'tools\source_version.txt'), "$Shared`n")
Write-Host "  OpenSmash-Ultimate's installer ($ref) is in place; it updates this folder now" -ForegroundColor Cyan
Write-Host ''

$pass = @($args | Where-Object { "$_" -ne '-NoUpdate' })
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script @pass
exit $LASTEXITCODE
