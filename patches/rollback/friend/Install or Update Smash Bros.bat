@echo off
rem SMASH BROS multiplayer (rollback netplay for WindowSmash): the one file friends download and click. It finds their
rem WindowSmash folder (built with their own game files), names it "SMASH BROS", downloads the multiplayer launcher
rem from GitHub and runs it once: that downloads the rollback source, builds build\ssbu_rollback.exe from their own
rem game files and makes the MULTIPLAYER shortcut. After that MULTIPLAYER updates itself. Nothing built from game
rem files is ever downloaded or shared. The PowerShell part is below the marker line.
set "RB_BAT=%~f0"
echo Installing SMASH BROS multiplayer (windows will ask you if anything is needed) ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "iex ((Get-Content -LiteralPath $env:RB_BAT -Raw) -split '#{2}POWERSHELL#{2}', 2)[1]"
if errorlevel 1 pause
exit /b
##POWERSHELL##
$OWNER = "Piaka11"; $REPO = "WindowSmash"; $BRANCH = "friends"
$ErrorActionPreference = "Stop"
$NAME = "SMASH BROS"
$memDir = Join-Path $env:LOCALAPPDATA "SMASH BROS Multiplayer"
$memFile = Join-Path $memDir "folder.txt"
Add-Type -AssemblyName System.Windows.Forms
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
function Say([string]$text, [string]$icon = "Information") {
    Write-Host $text; [void][System.Windows.Forms.MessageBox]::Show($owner, $text, "SMASH BROS Multiplayer", "OK", $icon)
}
function Ask([string]$text, [string]$buttons = "YesNo") {
    return [string][System.Windows.Forms.MessageBox]::Show($owner, $text, "SMASH BROS Multiplayer", $buttons, "Question")
}
function Fail([string]$text) { Say $text "Warning"; exit 1 }
function Has-Game([string]$p) { $p -and (Test-Path -LiteralPath (Join-Path $p "dump\modules\romfs.cfg")) }

# ---- which folder: remembered, found, or picked -----------------------------------------------------------------
$ws = ""
if (Test-Path -LiteralPath $memFile) { $m = (Get-Content -LiteralPath $memFile -TotalCount 1).Trim(); if (Has-Game $m) { $ws = $m } }
if (-not $ws) {
    $places = @([Environment]::GetFolderPath("Desktop"), [Environment]::GetFolderPath("MyDocuments"),
                (Join-Path $env:USERPROFILE "Downloads"), $env:USERPROFILE) + (Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Root })
    foreach ($pl in $places) {
        if (-not $pl -or -not (Test-Path -LiteralPath $pl)) { continue }
        $c = Join-Path $pl $NAME
        if (Has-Game $c) { if ((Ask "Found your game here:`n`n$c`n`nInstall multiplayer into this folder?") -eq "Yes") { $ws = $c }; break }
    }
}
if (-not $ws) {
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Choose your WindowSmash folder (the one with ""dump"" and ""launchers"" in it)."
    $dlg.ShowNewFolderButton = $false
    if ($dlg.ShowDialog($owner) -ne "OK") { Fail "No folder chosen: nothing was installed." }
    $ws = $dlg.SelectedPath
}
$ws = $ws.TrimEnd('\')
if (-not (Has-Game $ws)) { Fail "$ws has no game files (dump\modules\romfs.cfg).`n`nSet WindowSmash up with your own game files first (""Install WindowSmash.bat""), then run this again." }
if (-not (Test-Path -LiteralPath (Join-Path $ws "build\main"))) { Fail "WindowSmash isn't built in $ws yet.`n`nRun ""Install WindowSmash.bat"" there first (it builds the game), then run this again." }
if (Get-Process -Name ssbu_rollback, ssbu -ErrorAction SilentlyContinue) { Fail "The game is running: close it, then run this again." }

# ---- the folder's name: SMASH BROS (the same on every PC) -------------------------------------------------------
if ((Split-Path -Leaf $ws) -ne $NAME) {
    $to = Join-Path (Split-Path -Parent $ws) $NAME
    if (Test-Path -LiteralPath $to) {
        Say "There's already a folder named ""$NAME"" next to your WindowSmash folder, so the name stays ""$(Split-Path -Leaf $ws)""." "Warning"
    } else {
        while ($true) {
            try { Rename-Item -LiteralPath $ws -NewName $NAME; $ws = $to; break }
            catch { if ((Ask "The folder couldn't be renamed to ""$NAME"" (something has it open: close any window or program using it).`n`nRetry, or Cancel to keep its name." "RetryCancel") -ne "Retry") { break } }
        }
    }
}
# (the game files' locations are full paths in dump\modules\romfs*.cfg: point them at the folder as it is named now)
foreach ($cfg in "dump\modules\romfs.cfg", "dump\modules\romfs_base.cfg") {
    $f = Join-Path $ws $cfg
    if (Test-Path -LiteralPath $f) {
        $t = Get-Content -LiteralPath $f | ForEach-Object { $i = $_.IndexOf('\dump\modules\'); if ($i -ge 0) { $ws + $_.Substring($i) } else { $_ } }
        Set-Content -LiteralPath $f -Encoding ascii -Value $t
    }
}
New-Item -ItemType Directory -Force -Path $memDir | Out-Null
Set-Content -LiteralPath $memFile -Encoding utf8 -Value $ws

# ---- the multiplayer launcher from GitHub; it downloads and builds the rest --------------------------------------
$launcher = Join-Path $ws "launchers\smash_multiplayer.ps1"
try { Invoke-WebRequest "https://raw.githubusercontent.com/$OWNER/$REPO/$BRANCH/launchers/smash_multiplayer.ps1" -OutFile $launcher -UseBasicParsing -TimeoutSec 60 }
catch { Fail "GitHub couldn't be reached ($($_.Exception.Message)).`n`nCheck the internet connection and try again." }
& powershell -NoProfile -ExecutionPolicy Bypass -File $launcher -Install
if ($LASTEXITCODE -ne 0) { exit 1 }

Say "Installed!`n`nTo play: open your ""$(Split-Path -Leaf $ws)"" folder and double-click MULTIPLAYER.`n($ws)`n`nMULTIPLAYER updates itself when there's a new version: nothing else to download."
Start-Process explorer.exe "/select,""$(Join-Path $ws 'MULTIPLAYER.lnk')"""
exit 0
