# [rollback research build: build\ssbu_rollback.exe - see patches\rollback\README.md and TWO_PC_SETUP.md]
# The window "Rollback Netplay.bat" opens: Host or Join, with buttons, no command prompt. It starts the game the same
# way "Rollback Netplay (command prompt).bat" does (that one stays for tests: bots, a fixed delay).
#   powershell -File rollback_netplay.ps1 -Test host 3            (no window: what it would start, for checks)
#   powershell -File rollback_netplay.ps1 -Test join 192.168.1.20
param([string]$Test = "", [string]$Arg = "", [string]$Shot = "")   # (-Shot <folder>: each page drawn to a .png there, for checks)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# ---- starting the game (both ways: the window's buttons, and -Test) --------------------------------------------
function Start-Rollback([string]$role, [string]$value, [switch]$dry) {
    $exe = Join-Path $root "build\ssbu_rollback.exe"
    if (-not (Test-Path $exe)) { throw "build\ssbu_rollback.exe is missing." }
    if (-not (Test-Path (Join-Path $root "dump\modules\romfs.cfg"))) { throw "The game files aren't here (dump\modules\romfs.cfg is missing)." }
    if ($role -eq "host") {
        $local = 0; $port = 7701; $peerport = 7702; $peer = "*"; $log = "host"
        $env:SSBU_RB_NET_PLAYERS = $value
    } else {
        $local = -1; $port = 0; $peerport = 7701; $peer = ($value -replace '[\s"]', ''); $log = "join"
        # (each joining game its own log: another game on this PC may still be writing one)
        foreach ($n in "join", "join2", "join3", "join4") {
            $log = $n
            $f = Join-Path $root "build\play_rollback_net_$n.log"
            if (-not (Test-Path $f)) { break }
            try { $s = [IO.File]::Open($f, 'Append', 'Write', 'None'); $s.Close(); break } catch { }
        }
    }
    # (the game files' locations are written into dump\modules\romfs*.cfg as full paths: point them at this folder)
    if (-not $dry) {
        foreach ($cfg in "dump\modules\romfs.cfg", "dump\modules\romfs_base.cfg") {
            if (Test-Path $cfg) {
                $t = Get-Content $cfg | ForEach-Object { $i = $_.IndexOf('\dump\modules\'); if ($i -ge 0) { $root + $_.Substring($i) } else { $_ } }
                Set-Content -Encoding ascii $cfg $t
            }
        }
    }
    $delay = if ($env:RB_DELAY -and $env:RB_DELAY -ne "auto") { $env:RB_DELAY } else { "-1" }   # (tests only)
    $env:SSBU_RB = "1"
    $env:SSBU_RB_NET = "net,$local,$port,$peerport,$delay,7200,$peer"
    if (-not $env:SSBU_RB_NET_LIVE) { $env:SSBU_RB_NET_LIVE = "1" }   # (tests: 0, a bot)
    $env:SSBU_CONTROLLERS = Join-Path $root "controllers_rollback_p1.ini"
    $env:SSBU_DATA_ROOT = $null; $env:SSBU_INPUT = $null
    $logpath = "build\play_rollback_net_$log.log"
    if ($role -eq "host" -and (Test-Path "build\rollback_address.txt")) { if (-not $dry) { Remove-Item "build\rollback_address.txt" } }
    if ($dry) { return "SSBU_RB_NET=$($env:SSBU_RB_NET) players=$($env:SSBU_RB_NET_PLAYERS) log=$logpath" }
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c `"build\ssbu_rollback.exe dump\modules --status 30 > $logpath 2>&1`"" -WindowStyle Minimized
    return $logpath
}
# the host's addresses, once the game has said them (build\rollback_address.txt)
function Read-Addresses {
    $f = Join-Path $root "build\rollback_address.txt"
    if (-not (Test-Path $f)) { return $null }
    $text = Get-Content $f -Raw
    $lan = if ($text -match 'on this network:\s*([0-9.]+)') { $Matches[1] } elseif ($text -match 'Friends on this network join with:\s*([0-9.]+)') { $Matches[1] } else { "" }
    $out = if ($text -match 'elsewhere:\s*([0-9.]+)') { $Matches[1] } else { "" }
    return @{ Text = $text.Trim(); Lan = $lan; Outside = $out }
}

if ($Test) {   # (checks without a window)
    Start-Rollback $Test $Arg -dry
    exit 0
}

# ---- the window ------------------------------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$font = New-Object Drawing.Font("Segoe UI", 11)
$big = New-Object Drawing.Font("Segoe UI", 13, [Drawing.FontStyle]::Bold)
$form = New-Object Windows.Forms.Form
$form.Text = "Smash rollback netplay"
$form.ClientSize = New-Object Drawing.Size(460, 330)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"; $form.MaximizeBox = $false
$form.TopMost = $true   # (stays in front of the game's window, which opens over it, until closed: the host needs its addresses)
$form.Font = $font

function New-Label($text, $x, $y, $w = 420, $h = 28) {
    $l = New-Object Windows.Forms.Label; $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h); $l
}
function New-Button($text, $x, $y, $w, $h) {
    $b = New-Object Windows.Forms.Button; $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, $h); $b
}
function Show-Page($panel) { foreach ($p in $pages) { $p.Visible = ($p -eq $panel) } }
function Fail($msg) { [Windows.Forms.MessageBox]::Show($msg, "Smash rollback netplay", "OK", "Error") | Out-Null }

# page 1: host or join
$choose = New-Object Windows.Forms.Panel; $choose.Dock = "Fill"
$choose.Controls.Add((New-Label "Play a match with friends, each on their own PC." 20 20))
$hostBtn = New-Button "Host a match" 20 70 200 90; $hostBtn.Font = $big
$joinBtn = New-Button "Join a match" 240 70 200 90; $joinBtn.Font = $big
$choose.Controls.AddRange(@($hostBtn, $joinBtn))
$choose.Controls.Add((New-Label "Host: your PC runs the match and the others connect to it. Join: connect to a friend who hosts." 20 180 420 60))

# page 2: host
$hostPage = New-Object Windows.Forms.Panel; $hostPage.Dock = "Fill"
$hostPage.Controls.Add((New-Label "How many players, you included?" 20 20))
$count = New-Object Windows.Forms.ComboBox; $count.DropDownStyle = "DropDownList"; $count.Location = New-Object Drawing.Point(20, 55); $count.Width = 100
[void]$count.Items.AddRange(@("2", "3", "4")); $count.SelectedIndex = 1
$hostGo = New-Button "Start hosting" 20 100 200 50; $hostGo.Font = $big
$hostBack = New-Button "Back" 340 270 100 40
$hostPage.Controls.AddRange(@($count, $hostGo, $hostBack))

# page 3: the host's addresses
$addrPage = New-Object Windows.Forms.Panel; $addrPage.Dock = "Fill"
$addrInfo = New-Label "Starting the game and opening the router's port (up to a minute and a half)..." 20 15 420 50
$lanLabel = New-Label "Friends on the same network:" 20 75 260
$lanBox = New-Object Windows.Forms.TextBox; $lanBox.ReadOnly = $true; $lanBox.Location = New-Object Drawing.Point(20, 105); $lanBox.Width = 260
$lanCopy = New-Button "Copy" 300 102 90 32
$outLabel = New-Label "Friends elsewhere (internet):" 20 150 260
$outBox = New-Object Windows.Forms.TextBox; $outBox.ReadOnly = $true; $outBox.Location = New-Object Drawing.Point(20, 180); $outBox.Width = 260
$outCopy = New-Button "Copy" 300 177 90 32
$addrNote = New-Label "" 20 220 420 45
$addrDone = New-Button "Close" 340 270 100 40
$hostStatus = New-Label "" 20 258 310 66
$addrPage.Controls.AddRange(@($addrInfo, $lanLabel, $lanBox, $lanCopy, $outLabel, $outBox, $outCopy, $addrNote, $hostStatus, $addrDone))
foreach ($c in $lanLabel, $lanBox, $lanCopy, $outLabel, $outBox, $outCopy) { $c.Visible = $false }

# page 4: join
$joinPage = New-Object Windows.Forms.Panel; $joinPage.Dock = "Fill"
$joinPage.Controls.Add((New-Label "The host's address (the host's window shows it):" 20 20))
$addr = New-Object Windows.Forms.TextBox; $addr.Location = New-Object Drawing.Point(20, 55); $addr.Width = 300
$lastFile = Join-Path $root "build\rollback_last_host.txt"
if (Test-Path $lastFile) { $addr.Text = (Get-Content $lastFile -Raw).Trim() }
$joinGo = New-Button "Join" 20 100 200 50; $joinGo.Font = $big
$joinBack = New-Button "Back" 340 270 100 40
$joinPage.Controls.AddRange(@($addr, $joinGo, $joinBack))

# page 5: joined
$joinedPage = New-Object Windows.Forms.Panel; $joinedPage.Dock = "Fill"
$joinedPage.Controls.Add((New-Label "The game is starting. When it's up: Smash, then pick your fighter with your controller. The host picks the stage. Keep this window open: it says when the game waits for the others." 20 20 420 110))
$joinedDone = New-Button "Close" 340 270 100 40
$joinStatus = New-Label "" 20 150 420 60
$joinedPage.Controls.AddRange(@($joinedDone, $joinStatus))

$pages = @($choose, $hostPage, $addrPage, $joinPage, $joinedPage)
$form.Controls.AddRange($pages)
Show-Page $choose

$hostBtn.Add_Click({ Show-Page $hostPage })
$joinBtn.Add_Click({ Show-Page $joinPage; $addr.Focus() })
$hostBack.Add_Click({ Show-Page $choose })
$joinBack.Add_Click({ Show-Page $choose })
$addrDone.Add_Click({ $form.Close() })
$joinedDone.Add_Click({ $form.Close() })
$lanCopy.Add_Click({ if ($lanBox.Text) { [Windows.Forms.Clipboard]::SetText($lanBox.Text); $addrNote.Text = "Copied: send it to the friends on your network." } })
$outCopy.Add_Click({ if ($outBox.Text) { [Windows.Forms.Clipboard]::SetText($outBox.Text); $addrNote.Text = "Copied: send it to the friends elsewhere." } })

$script:waited = 0
$timer = New-Object Windows.Forms.Timer; $timer.Interval = 1000
$timer.Add_Tick({
    $script:waited++
    $a = Read-Addresses
    if ($a) {
        $timer.Stop()
        $addrInfo.Text = "Send your friends the address that fits, then start Smash and pick your fighter (and the stage)."
        foreach ($c in $lanLabel, $lanBox, $lanCopy) { $c.Visible = $true }
        $lanBox.Text = $a.Lan
        if ($a.Outside) { foreach ($c in $outLabel, $outBox, $outCopy) { $c.Visible = $true }; $outBox.Text = $a.Outside }
        else { $addrNote.Text = ($a.Text -replace "Friends on this network join with:\s*[0-9.]+\s*", "") }
    } elseif ($script:waited -ge 90) {
        $timer.Stop()
        $addrInfo.Text = "The game hasn't said its address yet: see build\rollback_address.txt in a minute."
    }
})
# what the game waits for at a match set-up (it draws nothing then, so it looks frozen): build\rollback_status.txt
foreach ($l in $hostStatus, $joinStatus) { $l.Font = New-Object Drawing.Font($form.Font.FontFamily, 11, [Drawing.FontStyle]::Bold); $l.ForeColor = [Drawing.Color]::DarkBlue }
$statusFile = Join-Path $root "build\rollback_status.txt"
$statusTimer = New-Object Windows.Forms.Timer; $statusTimer.Interval = 500
$statusTimer.Add_Tick({
    $t = ""; try { if (Test-Path $statusFile) { $t = ([IO.File]::ReadAllText($statusFile)).Trim() } } catch {}
    if (-not $t) { $t = "Keep this window open: it says here when the game waits for players." }
    $hostStatus.Text = $t; $joinStatus.Text = $t
})
function Start-Status { try { Remove-Item $statusFile -ErrorAction SilentlyContinue } catch {}; $statusTimer.Start() }
$hostGo.Add_Click({
    try { Start-Status; Start-Rollback "host" $count.SelectedItem | Out-Null; Show-Page $addrPage; $timer.Start() } catch { Fail $_.Exception.Message }
})
$joinGo.Add_Click({
    $a = $addr.Text.Trim()
    if (-not $a) { Fail "Type the host's address first."; return }
    try { Set-Content -Encoding ascii $lastFile $a; Start-Status; Start-Rollback "join" $a | Out-Null; Show-Page $joinedPage } catch { Fail $_.Exception.Message }
})
$addr.Add_KeyDown({ if ($_.KeyCode -eq "Enter") { $joinGo.PerformClick() } })

if ($Shot) {   # (checks: every page drawn to an image, the address page with sample addresses; no game started)
    $form.Show(); [Windows.Forms.Application]::DoEvents()
    foreach ($c in $lanLabel, $lanBox, $lanCopy, $outLabel, $outBox, $outCopy) { $c.Visible = $true }
    $hostStatus.Text = $joinStatus.Text = "Waiting for players to join... (1 of 3 players here)"
    $lanBox.Text = "192.168.1.20"; $outBox.Text = "203.0.113.7"
    $addrInfo.Text = "Send your friends the address that fits, then start Smash and pick your fighter (and the stage)."
    $i = 0
    foreach ($pg in $pages) {
        Show-Page $pg; [Windows.Forms.Application]::DoEvents()
        $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save((Join-Path $Shot "page$i.png")); $bmp.Dispose(); $i++
    }
    $form.Close(); exit 0
}
[void]$form.ShowDialog()
