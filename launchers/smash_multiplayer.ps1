# What the MULTIPLAYER shortcut runs (MULTIPLAYER.lnk in the WindowSmash folder, made by "Install or Update Smash Bros.bat"):
# asks GitHub whether the fork's latest release is a newer version than the one built here; if so, downloads its
# rollback files (source only: no game code is ever downloaded) and builds build\ssbu_rollback.exe from this PC's
# own game files; then opens the Host / Join window (rollback_netplay.ps1). Offline, or GitHub not answering:
# straight to the window with the version already built.
#   -Install   (the installer) always update and build, then make the MULTIPLAYER shortcuts
#   -Shortcuts (after an update: the new version of this script) only set the shortcuts right
param([switch]$Install, [switch]$Shortcuts)

$OWNER = "Piaka11"; $REPO = "WindowSmash"
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$stamp = Join-Path $root "patches\rollback\INSTALLED_COMMIT.txt"
$exe = Join-Path $root "build\ssbu_rollback.exe"
$log = Join-Path $root "build\rollback_update.log"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Say([string]$text, [string]$icon = "Information") {
    $o = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }
    [void][System.Windows.Forms.MessageBox]::Show($o, $text, "SMASH BROS Multiplayer", "OK", $icon)
}
function Ask([string]$text) {
    $o = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }
    return [string][System.Windows.Forms.MessageBox]::Show($o, $text, "SMASH BROS Multiplayer", "YesNo", "Question")
}

# ---- a small "please wait" window (the build takes minutes: friends must see something happening) ------------
$win = $null
function Show-Wait([string]$text) {
    if (-not $script:win) {
        $f = New-Object System.Windows.Forms.Form -Property @{
            Text = "SMASH BROS Multiplayer"; Width = 460; Height = 150; TopMost = $true; StartPosition = "CenterScreen"
            FormBorderStyle = "FixedDialog"; MaximizeBox = $false; MinimizeBox = $true; ControlBox = $false }
        $l = New-Object System.Windows.Forms.Label -Property @{ Left = 16; Top = 14; Width = 420; Height = 44 }
        $b = New-Object System.Windows.Forms.ProgressBar -Property @{ Left = 16; Top = 66; Width = 410; Height = 20; Style = "Marquee" }
        $f.Controls.AddRange(@($l, $b)); $f.Show()
        $script:win = @{ Form = $f; Label = $l }
    }
    $script:win.Label.Text = $text
    [System.Windows.Forms.Application]::DoEvents()
}
function Close-Wait { if ($script:win) { $script:win.Form.Close(); $script:win = $null } }

# ---- is there a newer version? -----------------------------------------------------------------------------------
$api = "https://api.github.com/repos/$OWNER/$REPO"
$have = if (Test-Path $stamp) { (Get-Content $stamp -TotalCount 1).Trim() } else { "" }
$latest = $null
# (the latest release: what has been tested; pushes to the branch alone change nobody's game)
try {
    if ($Shortcuts) { throw "only the shortcuts" }   # (straight after an update: nothing to ask GitHub)
    $tag = (Invoke-RestMethod "$api/releases/latest" -TimeoutSec 8 -Headers @{ "User-Agent" = "SmashBrosMultiplayer" }).tag_name
    $latest = (Invoke-RestMethod "$api/commits/$tag" -TimeoutSec 8 -Headers @{ "User-Agent" = "SmashBrosMultiplayer" }).sha
}
catch {
    # (installing with no release found: the branch itself; an update waits for a release)
    $err = $_.Exception.Message
    if ($Install) {
        try { $latest = (Invoke-RestMethod "$api/commits/rollback-netplay-preview" -TimeoutSec 8 -Headers @{ "User-Agent" = "SmashBrosMultiplayer" }).sha }
        catch { Say "Multiplayer couldn't be downloaded from GitHub ($err; $($_.Exception.Message)).`n`nCheck the internet connection and try again." "Warning"; exit 1 }
    }
}

if ($latest -and ($latest -ne $have -or -not (Test-Path $exe))) {
    $first = -not (Test-Path $exe)
    if (-not $Install -and -not $first) {
        Show-Wait "A new version of multiplayer is out: updating (about a minute) ..."
    } else {
        Show-Wait "Downloading multiplayer ..."
    }
    try {
        # (only this branch's rollback files: patches\rollback\, the rollback launchers, the controller settings)
        $tree = (Invoke-RestMethod "$api/git/trees/$latest`?recursive=1" -TimeoutSec 30 -Headers @{ "User-Agent" = "SmashBrosMultiplayer" }).tree
        $files = $tree | Where-Object { $_.type -eq "blob" -and (
            $_.path -like "patches/rollback/*" -or $_.path -eq "controllers_rollback_p1.ini" -or
            ($_.path -like "launchers/*" -and $_.path -match "(?i)rollback|multiplayer")) }
        if (-not $files) { throw "the branch has no multiplayer files" }
        $stage = Join-Path $root "build\rollback_update"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        foreach ($f in $files) {
            $to = Join-Path $stage ($f.path -replace "/", "\")
            New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
            $url = "https://raw.githubusercontent.com/$OWNER/$REPO/$latest/" + (($f.path -split "/" | ForEach-Object { [Uri]::EscapeDataString($_) }) -join "/")
            Invoke-WebRequest $url -OutFile $to -UseBasicParsing -TimeoutSec 60
            # (GitHub serves the stored LF; .bat files need CRLF, as a checkout gives them)
            if ($to -match "\.(bat|ps1)$") { [IO.File]::WriteAllText($to, ([IO.File]::ReadAllText($to) -replace "`r?`n", "`r`n")) }
            [System.Windows.Forms.Application]::DoEvents()
        }
        # (everything downloaded: only now put it in place)
        & robocopy $stage $root /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "the files couldn't be put in place (is a file in use?)" }
        Remove-Item $stage -Recurse -Force
    } catch {
        Close-Wait
        if ($Install -or $first) { Say "Multiplayer couldn't be downloaded ($($_.Exception.Message)).`n`nCheck the internet connection and try again." "Warning"; exit 1 }
        $latest = $null   # (play the version already built)
    }

    if ($latest) {
        Show-Wait $(if ($first) { "Building multiplayer from your game files.`nThe first time takes several minutes: you can keep using the PC." } else { "Building the update from your game files (about a minute) ..." })
        $p = Start-Process cmd.exe -ArgumentList "/c `"patches\rollback\build_rollback.bat > `"$log`" 2>&1`"" -WorkingDirectory $root -WindowStyle Hidden -PassThru
        while (-not $p.HasExited) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 200 }
        Close-Wait
        if ($p.ExitCode -ne 0) {
            $why = Get-Content $log -Raw -ErrorAction SilentlyContinue
            $hint = if ($why -match "changed since the rollback patch was made") { "Your WindowSmash is a different version from the one multiplayer was made for. Tell the friend who sent you this." }
                    elseif ($why -match "Build WindowSmash first") { "WindowSmash isn't built on this PC yet: run ""Install WindowSmash.bat"" first, then try again." }
                    else { "Send the friend who sent you this the file build\rollback_update.log." }
            if ((Test-Path $exe) -and -not $Install) {
                if ((Ask "The update couldn't be built.`n$hint`n`nPlay with the version you had? (Everyone in a match needs the same version.)") -ne "Yes") { exit 1 }
            } else { Say "Multiplayer couldn't be built.`n$hint" "Warning"; exit 1 }
        } else {
            Set-Content $stamp $latest -Encoding ascii
            & powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $PSCommandPath -Shortcuts   # (the version just downloaded)
        }
    }
}
Close-Wait

# ---- the MULTIPLAYER shortcuts, set right on every start: in this folder, and on the desktop if wanted -----------
# (a moved or renamed folder, or an older install's shortcut pointing elsewhere, is fixed here. The desktop one is
# asked once, Yes or No; patches\rollback\DESKTOP_SHORTCUT_MADE.txt keeps the answer, and a deleted one stays deleted)
function Set-Shortcut([string]$path) {
    try {
        $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        $want = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""$PSCommandPath"""
        $s = (New-Object -ComObject WScript.Shell).CreateShortcut($path)
        if ((Test-Path -LiteralPath $path) -and $s.TargetPath -eq $ps -and $s.Arguments -eq $want -and $s.WorkingDirectory -eq $root) { return }
        $s.TargetPath = $ps; $s.Arguments = $want; $s.WorkingDirectory = $root; $s.WindowStyle = 7
        $ico = Join-Path $root "docs\assets\windowsmash.ico"
        if (Test-Path $ico) { $s.IconLocation = "$ico,0" }
        $s.Description = "Play Smash online with friends (Host or Join)"
        $s.Save()
    } catch {}
}
function Set-Shortcuts {
    Set-Shortcut (Join-Path $root "MULTIPLAYER.lnk")
    $desk = [Environment]::GetFolderPath("Desktop")
    if (-not $desk) { return }
    $onDesk = Join-Path $desk "SMASH BROS MULTIPLAYER.lnk"
    $note = Join-Path $root "patches\rollback\DESKTOP_SHORTCUT_MADE.txt"
    if (Test-Path -LiteralPath $onDesk) { Set-Shortcut $onDesk; return }   # (there: kept pointing here)
    if (Test-Path $note) { return }   # (asked before: the answer stands)
    $yes = (Ask "Put a SMASH BROS MULTIPLAYER shortcut on your desktop too?`n`n(The MULTIPLAYER shortcut in your game folder is always there.)") -eq "Yes"
    if ($yes) { Set-Shortcut $onDesk }
    try { Set-Content $note $(if ($yes) { "Desktop shortcut: yes (deleting it keeps it deleted)." } else { "Desktop shortcut: no." }) -Encoding ascii } catch {}
}
Set-Shortcuts
if ($Install -or $Shortcuts) { exit 0 }

if (-not (Test-Path $exe)) { Say "Multiplayer isn't built yet. Run ""Install or Update Smash Bros.bat"" again." "Warning"; exit 1 }
& powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File (Join-Path $PSScriptRoot "rollback_netplay.ps1")
