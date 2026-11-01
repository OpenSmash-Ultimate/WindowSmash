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
$logFile = Join-Path $memDir "install.log"
New-Item -ItemType Directory -Force -Path $memDir | Out-Null
try { Start-Transcript -LiteralPath $logFile -Force | Out-Null } catch {}
Add-Type -AssemblyName System.Windows.Forms
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
function Say([string]$text, [string]$icon = "Information") {
    Write-Host $text; [void][System.Windows.Forms.MessageBox]::Show($owner, $text, "SMASH BROS Multiplayer", "OK", $icon)
}
function Ask([string]$text, [string]$buttons = "YesNo") {
    Write-Host "? $text"
    $a = [string][System.Windows.Forms.MessageBox]::Show($owner, $text, "SMASH BROS Multiplayer", $buttons, "Question")
    Write-Host "  -> $a"; return $a
}
function Fail([string]$text) {
    Say "$text`n`n(What happened is written in $($logFile): send it to the friend who sent you this.)" "Warning"
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
function Has-Game([string]$p) { $p -and (Test-Path -LiteralPath (Join-Path $p "dump\modules\romfs.cfg")) }
# (a folder picked "close" to the game: the folder itself, one above it, or up to two below it, e.g. a Downloads
# folder holding "SSBU\WindowSmash-main"; game folders found nearest first)
function Find-Game([string]$p) {
    $found = @()
    if (-not $p -or -not (Test-Path -LiteralPath $p)) { return $found }
    if (Has-Game $p) { return @($p) }
    $up = Split-Path -Parent $p
    if (Has-Game $up) { $found += $up }
    try {
        $kids = @(Get-ChildItem -LiteralPath $p -Directory -ErrorAction SilentlyContinue)
        foreach ($k in $kids) { if (Has-Game $k.FullName) { $found += $k.FullName } }
        foreach ($k in $kids) {
            foreach ($g in @(Get-ChildItem -LiteralPath $k.FullName -Directory -ErrorAction SilentlyContinue)) { if (Has-Game $g.FullName) { $found += $g.FullName } }
        }
    } catch {}
    return @($found | Select-Object -Unique)
}
function Pick-Folder {
    while ($true) {
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "Choose your WindowSmash folder (the one with ""dump"" and ""launchers"" in it). The folder just above it works too."
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog($owner) -ne "OK") { Fail "No folder chosen: nothing was installed." }
        Write-Host "picked: $($dlg.SelectedPath)"
        $c = @(Find-Game $dlg.SelectedPath)
        foreach ($g in $c) { if ((Ask "Found your game here:`n`n$g`n`nInstall multiplayer into this folder?") -eq "Yes") { return $g } }
        if (-not $c) {
            if ((Ask "No game files in or near:`n`n$($dlg.SelectedPath)`n`n(The right folder has ""dump"" and ""launchers"" in it.) Choose another folder?" "RetryCancel") -ne "Retry") { Fail "Nothing was installed." }
        }
    }
}

# ---- which folder: remembered, found, or picked -----------------------------------------------------------------
$ws = ""
if (Test-Path -LiteralPath $memFile) {
    $m = (Get-Content -LiteralPath $memFile -TotalCount 1).Trim()
    if ((Has-Game $m) -and (Ask "Update multiplayer in this folder?`n`n$m`n`n(No: choose another folder)") -eq "Yes") { $ws = $m }
}
if (-not $ws -and -not (Test-Path -LiteralPath $memFile)) {
    $places = @([Environment]::GetFolderPath("Desktop"), [Environment]::GetFolderPath("MyDocuments"),
                (Join-Path $env:USERPROFILE "Downloads"), $env:USERPROFILE) + (Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Root })
    foreach ($pl in $places) {
        if (-not $pl -or -not (Test-Path -LiteralPath $pl)) { continue }
        $c = Join-Path $pl $NAME
        if (Has-Game $c) { if ((Ask "Found your game here:`n`n$c`n`nInstall multiplayer into this folder?") -eq "Yes") { $ws = $c }; break }
    }
}
if (-not $ws) { $ws = Pick-Folder }
$ws = $ws.TrimEnd('\')
Write-Host "folder: $ws"
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
Set-Content -LiteralPath $memFile -Encoding utf8 -Value $ws

# ---- the multiplayer launcher from GitHub; it downloads and builds the rest --------------------------------------
# (the latest release: the tested version; without one, the branch. The message says what GitHub answered)
function Why($e) {
    $r = $e.Exception.Response
    if ($r) { return "GitHub answered $([int]$r.StatusCode) $($r.StatusDescription) for $($r.ResponseUri)" }
    return "no answer: $($e.Exception.Message)"
}
$launcher = Join-Path $ws "launchers\smash_multiplayer.ps1"
$h = @{ "User-Agent" = "SmashBrosMultiplayer" }
while ($true) {
    $why = @()
    $ref = $null
    try { $ref = (Invoke-RestMethod "https://api.github.com/repos/$OWNER/$REPO/releases/latest" -TimeoutSec 15 -Headers $h).tag_name }
    catch { $why += "release: $(Why $_)"; $ref = $BRANCH }
    try {
        Invoke-WebRequest "https://raw.githubusercontent.com/$OWNER/$REPO/$ref/launchers/smash_multiplayer.ps1" -OutFile $launcher -UseBasicParsing -TimeoutSec 60
        Write-Host "launcher from $ref"; break
    } catch { $why += "launcher ($ref): $(Why $_)" }
    $all = $why -join "`n"
    Write-Host $all
    $tip = if ($all -match "no answer") { "Check the internet connection (or a firewall/antivirus blocking PowerShell)." } else { "GitHub is up, but the files weren't there: tell the friend who sent you this." }
    if ((Ask "Multiplayer couldn't be downloaded.`n`n$all`n`n$tip`n`nTry again?" "RetryCancel") -ne "Retry") { Fail "Multiplayer couldn't be downloaded." }
}
try { Stop-Transcript | Out-Null } catch {}
& powershell -NoProfile -ExecutionPolicy Bypass -File $launcher -Install
if ($LASTEXITCODE -ne 0) { exit 1 }

Say "Installed!`n`nTo play: open your ""$(Split-Path -Leaf $ws)"" folder and double-click MULTIPLAYER.`n($ws)`n`nMULTIPLAYER updates itself when there's a new version: nothing else to download."
Start-Process explorer.exe "/select,""$(Join-Path $ws 'MULTIPLAYER.lnk')"""
exit 0
