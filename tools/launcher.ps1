<#
.SYNOPSIS
The window Play SSBU.bat opens before the game: the game version, online, display and controller
settings, then Play.

.DESCRIPTION
It edits the same files the game's settings menu (Esc) and players edit by hand, in place, keeping
their comments and every line it does not change:
  display.ini       window, width, height, pacing, render_scale, gpu, hide_cursor, overlay
  controllers.ini   [general] keyboard_player, background
  online_server.txt the first line that is not a '#' comment: host:port [lobby code], or off
  online_name.txt   the online name (empty: the game picks one at its first online start)
  online_direct.txt on / off: direct connections
  launcher.ini      show_launcher = 0: Play SSBU.bat starts the game directly (hold Shift for this window)
Play saves only the values changed in the window, then starts what Play SSBU.bat starts: the game
with --online, its output in build\play.log. Close starts nothing and saves nothing. The game
version shown is version.txt's (what the build wrote: 13.0.5).

The ini writer follows the game's own (runtime/hle_settings.c, settings_ini_set) byte for byte:
tests/test_launcher_ini.py checks the two give the same files.

Exit codes: 0 played or closed, 1 the game isn't built here, 2 the window could not be opened
(Play SSBU.bat then starts the game the old way).

.PARAMETER Auto
What Play SSBU.bat passes: start the game directly when launcher.ini says show_launcher = 0 and
Shift is not held; otherwise open the window.
.PARAMETER DryRun
Print the files Play would change and the command line it would start, and change or start nothing.
.PARAMETER NoWindow
Define the functions and stop (tests dot-source this file with it).
.PARAMETER Root
The port's folder (default: this file's folder's parent).
#>
param([switch]$Auto, [switch]$DryRun, [switch]$NoWindow, [string]$Root)

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path   # (.NET's file calls want it whole)

$PublicLobby = 'smash.recomps.wtf:39421'   # (the online_server.txt the port comes with)
# The game's own values when a file or key is missing (runtime/gpu_display.c, hle_pad.c).
$Defaults = @{
    window = 'windowed'; width = '1280'; height = '720'; pacing = 'clock'; latency = '3'
    hide_cursor = '1'; overlay = '1'; gpu = 'auto'; render_scale = '1'; keyboard_player = '1'; background = '1'
}
# What Play SSBU.bat clears, so the files count (the variables win over them in the game).
$ClearedVars = @('SSBU_DATA_ROOT', 'SSBU_INPUT', 'SSBU_NET_HOST', 'SSBU_NET_RELAY', 'SSBU_NET_BIND',
                 'SSBU_NET_ROOM', 'SSBU_NET_NAME', 'SSBU_NET_DIRECT', 'SSBU_NET_PORT')

# ---- files as lines (as hle_settings.c: bytes kept, line ends kept, no BOM) -----------------
# Text is held as Latin-1 strings, one char per byte, so every byte comes back as it was; a new
# value is put in as its UTF-8 bytes.
$Latin1 = [Text.Encoding]::GetEncoding(28591)

function Read-TextLines([string]$Path) {
    <# @{Lines; Crlf}; a missing file is empty, with Windows line ends. #>
    $t = @{ Lines = New-Object Collections.Generic.List[string]; Crlf = $true }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $t }
    $s = $Latin1.GetString([IO.File]::ReadAllBytes($Path))
    if ($s.Length -ge 3 -and $s[0] -eq [char]0xEF -and $s[1] -eq [char]0xBB -and $s[2] -eq [char]0xBF) { $s = $s.Substring(3) }   # (Notepad's BOM)
    $p = 0
    $seen = $false
    while ($p -lt $s.Length) {
        $nl = $s.IndexOf([char]10, $p)
        $e = if ($nl -ge 0) { $nl } else { $s.Length }
        $line = $s.Substring($p, $e - $p)
        $cr = $line.Length -and $line[$line.Length - 1] -eq [char]13
        if ($nl -ge 0 -and -not $seen) { $seen = $true; $t.Crlf = $cr }
        if ($cr) { $line = $line.Substring(0, $line.Length - 1) }
        $t.Lines.Add($line)
        $p = if ($nl -ge 0) { $nl + 1 } else { $s.Length }
    }
    return $t
}

function Write-TextLines([string]$Path, $Text) {
    <# Writes <file>.tmp, then puts it in the file's place. #>
    $eol = if ($Text.Crlf) { "`r`n" } else { "`n" }
    $sb = New-Object Text.StringBuilder
    foreach ($l in $Text.Lines) { [void]$sb.Append($l).Append($eol) }
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllBytes($tmp, $Latin1.GetBytes($sb.ToString()))
    if (Test-Path -LiteralPath $Path -PathType Leaf) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) }
    else { [IO.File]::Move($tmp, $Path) }
}

function ConvertTo-Bytes([string]$s) { return $Latin1.GetString([Text.Encoding]::UTF8.GetBytes($s)) }
function ConvertFrom-Bytes([string]$s) { return [Text.Encoding]::UTF8.GetString($Latin1.GetBytes($s)) }

function Skip-Space([string]$s) { return $s.TrimStart(' ', "`t") }
function Test-Blank([string]$s) { return (Skip-Space $s).Length -eq 0 }

function Get-HeaderName([string]$s) {
    <# [name] -> name with its spaces made single; $null if not a header. #>
    $s = Skip-Space $s
    if (-not $s.Length -or $s[0] -ne '[') { return $null }
    $e = $s.IndexOf([char]']')
    if ($e -lt 0) { return $null }
    $sb = New-Object Text.StringBuilder
    $space = $false
    $i = 1
    while ($i -lt $e -and ($s[$i] -eq ' ' -or $s[$i] -eq "`t")) { $i++ }
    for (; $i -lt $e; $i++) {
        $c = $s[$i]
        if ($c -eq ' ' -or $c -eq "`t") { $space = $true; continue }
        if ($space -and $sb.Length) { [void]$sb.Append(' ') }
        $space = $false
        [void]$sb.Append($c)
    }
    return $sb.ToString()
}

function Get-LineKey([string]$s) {
    <# The key of a key = value line, or $null. #>
    $s = Skip-Space $s
    if ($s.Length -eq 0 -or $s[0] -eq ';' -or $s[0] -eq '#' -or $s[0] -eq '[') { return $null }
    $eq = $s.IndexOf([char]'=')
    if ($eq -lt 0) { return $null }
    $k = $s.Substring(0, $eq).TrimEnd(' ', "`t")
    if ($k.Length -eq 0) { return $null }
    return $k
}

function Get-CommentStart([string]$v) {
    <# Where a value's own comment starts (';' or '#' after a space or tab), or its length. #>
    for ($p = 1; $p -lt $v.Length; $p++) {
        if (($v[$p] -eq ';' -or $v[$p] -eq '#') -and ($v[$p - 1] -eq ' ' -or $v[$p - 1] -eq "`t")) { return $p }
    }
    return $v.Length
}

function Find-IniKey($Text, [string]$Section, [string]$Key) {
    <# @{Line; Last}: the key's line in the section (flat: anywhere) or -1, and the section's last
       non-blank line (-1: no such section). #>
    $lines = $Text.Lines
    $in = -not $Section
    $last = if ($Section) { -1 } else { $lines.Count - 1 }
    $want = if ($Section) { Get-HeaderName "[$Section]" } else { $null }
    $found = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($Section) {
            $name = Get-HeaderName $lines[$i]
            if ($null -ne $name) {
                $in = [string]::Equals($name, $want, [StringComparison]::OrdinalIgnoreCase)
                if ($in) { $last = $i }
                continue
            }
        }
        if (-not $in) { continue }
        if ($Section -and -not (Test-Blank $lines[$i])) { $last = $i }
        if ($found -lt 0) {
            $k = Get-LineKey $lines[$i]
            if ($null -ne $k -and [string]::Equals($k, $Key, [StringComparison]::OrdinalIgnoreCase)) { $found = $i }
        }
    }
    return @{ Line = $found; Last = $last }
}

function Get-IniValue([string]$Path, [string]$Section, [string]$Key) {
    <# key = value from an ini file ($Section empty: a flat file), its comment dropped; $null if missing. #>
    $t = Read-TextLines $Path
    $f = Find-IniKey $t $Section $Key
    if ($f.Line -lt 0) { return $null }
    $line = $t.Lines[$f.Line]
    $v = Skip-Space $line.Substring($line.IndexOf([char]'=') + 1)
    return ConvertFrom-Bytes $v.Substring(0, (Get-CommentStart $v)).TrimEnd(' ', "`t")
}

function Set-IniValue([string]$Path, [string]$Section, [string]$Key, $Value) {
    <# Sets key = value in place (keeping the comment after the value), or adds it at the end of its
       section ([section] made at the end of the file when missing). $Section empty: a flat file.
       $Value $null removes the key. Nothing is written when nothing changes. Returns $true when
       the file changed. #>
    $t = Read-TextLines $Path
    $f = Find-IniKey $t $Section $Key
    $i = $f.Line
    if ($null -ne $Value) { $Value = ConvertTo-Bytes ([string]$Value) }
    if ($i -ge 0 -and $null -eq $Value) { $t.Lines.RemoveAt($i) }
    elseif ($i -ge 0) {
        $line = $t.Lines[$i]
        $eq = $line.IndexOf([char]'=')
        $rest = $line.Substring($eq + 1)
        $v = Skip-Space $rest
        $c = Get-CommentStart $v
        $ws = $c
        while ($ws -gt 0 -and ($v[$ws - 1] -eq ' ' -or $v[$ws - 1] -eq "`t")) { $ws-- }
        $tail = if ($c -lt $v.Length) { $v.Substring($ws) } else { '' }   # (no comment: trailing spaces go)
        $new = $line.Substring(0, $eq + 1) + ' ' + $Value + $tail
        if ([string]::Equals($new, $line)) { return $false }
        $t.Lines[$i] = $new
    } elseif ($null -ne $Value) {
        $new = "$Key = $Value"
        if ($f.Last -ge 0) { $t.Lines.Insert($f.Last + 1, $new) }
        else {
            if ($t.Lines.Count -and -not (Test-Blank $t.Lines[$t.Lines.Count - 1])) { $t.Lines.Add('') }
            if ($Section) { $t.Lines.Add("[$Section]") }
            $t.Lines.Add($new)
        }
    } else { return $false }
    Write-TextLines $Path $t
    return $true
}

# ---- one-line files (online_*.txt): the first line neither empty nor a '#' comment ----------
function Find-FirstLine($Text) {
    for ($i = 0; $i -lt $Text.Lines.Count; $i++) {
        $s = Skip-Space $Text.Lines[$i]
        if ($s.Length -and $s[0] -ne '#') { return $i }
    }
    return -1
}

function Get-LineValue([string]$Path) {
    <# That line, trimmed; $null when there is none. #>
    $t = Read-TextLines $Path
    $i = Find-FirstLine $t
    if ($i -lt 0) { return $null }
    return ConvertFrom-Bytes (Skip-Space $t.Lines[$i]).TrimEnd(' ', "`t")
}

function Set-LineValue([string]$Path, [string]$Line) {
    <# Replaces that line (or adds it at the end), keeping the comments. Returns $true when the file changed. #>
    $t = Read-TextLines $Path
    $Line = ConvertTo-Bytes $Line
    $i = Find-FirstLine $t
    if ($i -ge 0 -and [string]::Equals($t.Lines[$i], $Line)) { return $false }
    if ($i -ge 0) { $t.Lines[$i] = $Line } else { $t.Lines.Add($Line) }
    Write-TextLines $Path $t
    return $true
}

# ---- the game ------------------------------------------------------------------------------
$NotBuilt = "The game isn't built here yet: run Install WindowSmash.bat."

function Test-GameBuilt([string]$Dir) {
    return Test-Path -LiteralPath (Join-Path $Dir 'build\ssbu.exe') -PathType Leaf
}

function Get-GameVersion([string]$Dir) {
    <# The game version the build wrote: version.txt's first line that is neither empty nor a
       comment, or '' (none written yet). #>
    $file = Join-Path $Dir 'version.txt'
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        foreach ($l in [IO.File]::ReadAllLines($file, [Text.Encoding]::Default)) {
            if ($l.Length -and $l[0] -ne ';' -and $l[0] -ne '#') { return $l }
        }
    }
    return ''
}

function Get-GameCommand {
    <# What Play SSBU.bat runs, inside start "SSBU log" /min cmd /c "...". #>
    return 'build\ssbu.exe dump\modules --status 30 --online > build\play.log 2>&1'
}

function Start-Game {
    $line = "start `"SSBU log`" /min cmd /c `"$(Get-GameCommand)`""
    if ($DryRun) { Write-Host "would start, in $Root`: $line"; return }
    foreach ($v in $ClearedVars) { Remove-Item -LiteralPath "Env:$v" -ErrorAction SilentlyContinue }
    Start-Process -FilePath $env:ComSpec -ArgumentList "/c $line" -WorkingDirectory $Root -WindowStyle Hidden
}

function Get-Number([string]$s) {
    <# The number a value starts with, as the game's atoi reads it (0 when none). #>
    if ($s -match '^[ \t]*([+-]?\d{1,9})') { return [int]$Matches[1] }
    return 0
}

function Test-ShowLauncher {
    <# launcher.ini's show_launcher: missing or anything but 0 shows the window. #>
    $v = Get-IniValue (Join-Path $Root 'launcher.ini') '' 'show_launcher'
    return $null -eq $v -or $v -eq '' -or (Get-Number $v) -ne 0
}

if ($NoWindow) { return }

# ---- the window ------------------------------------------------------------------------------
try {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
} catch {
    Write-Host "The launcher window cannot open here ($($_.Exception.Message))."
    exit 2
}

$built = Test-GameBuilt $Root

if ($Auto -and -not (Test-ShowLauncher) -and -not ([Windows.Forms.Control]::ModifierKeys -band [Windows.Forms.Keys]::Shift)) {
    if (-not $built) { Write-Host $NotBuilt; exit 1 }
    Start-Game
    exit 0
}

$displayIni = Join-Path $Root 'display.ini'
$padsIni = Join-Path $Root 'controllers.ini'
$launcherIni = Join-Path $Root 'launcher.ini'
$serverFile = Join-Path $Root 'online_server.txt'
$nameFile = Join-Path $Root 'online_name.txt'
$directFile = Join-Path $Root 'online_direct.txt'

function Get-Setting([string]$Path, [string]$Section, [string]$Key) {
    <# The value the game uses: the file's, else its default. #>
    $v = Get-IniValue $Path $Section $Key
    if ($null -eq $v -or $v -eq '') { return $Defaults[$Key] }
    return $v
}

# online_server.txt: host:port [lobby code], or off
$serverWords = @(([string](Get-LineValue $serverFile)).Trim() -split '[ \t]+') + @('', '')
$lobbyHost = $serverWords[0]
$lobbyCode = $serverWords[1]
$netMode = if (-not $lobbyHost -or $lobbyHost -eq 'off') { 'offline' }
           elseif ($lobbyHost -eq $PublicLobby) { if ($lobbyCode) { 'private' } else { 'public' } }
           else { 'custom' }

try {
[Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object Windows.Forms.Form
$form.Text = 'WindowSmash'
$form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
$form.ClientSize = New-Object Drawing.Size(760, 574)
$form.FormBorderStyle = [Windows.Forms.FormBorderStyle]::FixedDialog
$form.MaximizeBox = $false
$form.StartPosition = [Windows.Forms.FormStartPosition]::CenterScreen
$form.Font = New-Object Drawing.Font('Segoe UI', 9.75)
$ico = Join-Path $Root 'docs\assets\windowsmash.ico'
if (Test-Path -LiteralPath $ico) { $form.Icon = New-Object Drawing.Icon($ico) }
$tips = New-Object Windows.Forms.ToolTip
$tips.AutoPopDelay = 20000
$boldFont = New-Object Drawing.Font('Segoe UI', 9.75, [Drawing.FontStyle]::Bold)

function Add-Control($Control, [int]$X, [int]$Y, [int]$W, [int]$H, [string]$Tip) {
    $Control.Location = New-Object Drawing.Point($X, $Y)
    $Control.Size = New-Object Drawing.Size($W, $H)
    if ($Tip) { $tips.SetToolTip($Control, $Tip) }
    $form.Controls.Add($Control)
    return $Control
}
function New-Text([string]$Text, [int]$X, [int]$Y, [int]$W, [int]$H = 20, [switch]$Bold, [switch]$Dim) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $Text
    if ($Bold) { $l.Font = $boldFont }
    if ($Dim) { $l.ForeColor = [Drawing.Color]::DimGray }
    return Add-Control $l $X $Y $W $H
}
function New-Choice([object[]]$Items, [string]$Selected, [int]$X, [int]$Y, [int]$W, [string]$Tip) {
    <# A drop-down list of @{Value; Text}, with the one whose Value is $Selected chosen. #>
    $c = New-Object Windows.Forms.ComboBox
    $c.DropDownStyle = [Windows.Forms.ComboBoxStyle]::DropDownList
    $c.DisplayMember = 'Text'
    foreach ($it in $Items) { [void]$c.Items.Add([pscustomobject]$it) }
    if ($c.Items.Count) { $c.SelectedIndex = 0 }
    for ($i = 0; $i -lt $c.Items.Count; $i++) {
        if ([string]::Equals($c.Items[$i].Value, $Selected, [StringComparison]::OrdinalIgnoreCase)) { $c.SelectedIndex = $i }
    }
    $c.DropDownWidth = [Math]::Max($W, 300)
    return Add-Control $c $X $Y $W 26 $Tip
}
function New-Check([string]$Text, [bool]$On, [int]$X, [int]$Y, [int]$W, [string]$Tip) {
    $cb = New-Object Windows.Forms.CheckBox
    $cb.Text = $Text
    $cb.Checked = $On
    return Add-Control $cb $X $Y $W 24 $Tip
}
function New-Number([int]$Value, [int]$Min, [int]$Max, [int]$X, [int]$Y, [int]$W, [string]$Tip) {
    $n = New-Object Windows.Forms.NumericUpDown
    $n.Minimum = $Min
    $n.Maximum = $Max
    $n.Value = [Math]::Min($Max, [Math]::Max($Min, $Value))
    return Add-Control $n $X $Y $W 26 $Tip
}
function New-TextBox([string]$Text, [int]$MaxLength, [int]$X, [int]$Y, [int]$W, [string]$Tip) {
    $b = New-Object Windows.Forms.TextBox
    if ($MaxLength) { $b.MaxLength = $MaxLength }
    $b.Text = $Text
    return Add-Control $b $X $Y $W 26 $Tip
}

# Game version
[void](New-Text 'Game version' 16 14 120 -Bold)
$gameVersion = Get-GameVersion $Root
[void](New-Text $(if (-not $built) { 'not built yet' } elseif ($gameVersion) { $gameVersion } else { 'built' }) 140 14 604)

# Display (left column)
$L = 16; $LC = 140; $LW = 220
[void](New-Text 'Display' $L 56 344 -Bold)
[void](New-Text 'Window' $L 87 120)
$windowBox = New-Choice @(
    @{ Value = 'windowed'; Text = 'A window' },
    @{ Value = 'borderless'; Text = 'Borderless (whole monitor)' },
    @{ Value = 'fullscreen'; Text = 'Full screen' }) (Get-Setting $displayIni '' 'window') $LC 84 $LW `
    "A window: you can resize it; the picture stays 16:9.`nBorderless: no frame, covering the monitor the window is on.`nFull screen: borderless and always on top.`nF11 or Alt+Enter switches while playing."
[void](New-Text 'Window size' $L 121 120)
$sizeTip = 'The inside of the window, at 100% display scaling (Windows scales it with its own setting).'
$widthBox = New-Number (Get-Number (Get-Setting $displayIni '' 'width')) 320 7680 $LC 118 98 $sizeTip
[void](New-Text 'x' ($LC + 104) 121 14)
$heightBox = New-Number (Get-Number (Get-Setting $displayIni '' 'height')) 180 4320 ($LC + 122) 118 98 $sizeTip
[void](New-Text 'Frame pacing' $L 155 120)
$pacingBox = New-Choice @(
    @{ Value = 'clock'; Text = 'Steady 60 (any monitor)' },
    @{ Value = 'vrr'; Text = 'G-Sync / FreeSync monitor' },
    @{ Value = 'display'; Text = "Follow the monitor's refresh" }) (Get-Setting $displayIni '' 'pacing') $LC 152 $LW `
    "Steady 60: 60 frames a second, each shown at the monitor's next refresh.`nG-Sync / FreeSync: each frame shown the moment it is ready; best borderless. Tears without G-Sync / FreeSync.`nFollow the monitor: its own rate (60, 120 or 240 Hz only), e.g. 59.94 frames a second."
[void](New-Text 'Render scale' $L 189 120)
# (as the game reads it: auto, or a number from 0.5 to 4; anything else is 1)
$scale = (Get-Setting $displayIni '' 'render_scale').Trim().ToLowerInvariant()
$scaleNumber = 0.0
if ($scale -ne 'auto') {
    if ([double]::TryParse($scale, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$scaleNumber) -and
        $scaleNumber -ge 0.5 -and $scaleNumber -le 4) { $scale = $scaleNumber.ToString([Globalization.CultureInfo]::InvariantCulture) }
    else { $scale = '1' }
}
$scaleItems = @(
    @{ Value = '1'; Text = "1: 1920x1080 (the game's own)" },
    @{ Value = 'auto'; Text = 'Auto: as big as the window' },
    @{ Value = '1.5'; Text = '1.5: 2880x1620' },
    @{ Value = '2'; Text = '2: 3840x2160 (4K)' },
    @{ Value = '3'; Text = '3: 5760x3240' })
if (-not ($scaleItems | Where-Object { $_.Value -eq $scale })) { $scaleItems += @{ Value = $scale; Text = "$scale (from display.ini)" } }
$scaleBox = New-Choice $scaleItems $scale $LC 186 $LW `
    "How big the game draws its pictures. Bigger is sharper on a big screen and more work for the graphics card (2 = four times the pixels of 1).`nAuto: as many pixels as the game's window has when the game starts, at least 1."
[void](New-Text 'Graphics card' $L 223 120)
$gpu = Get-Setting $displayIni '' 'gpu'
$gpuItems = @(@{ Value = 'auto'; Text = 'The fastest (auto)' })
try {
    foreach ($n in @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name } | Where-Object { $_ } | Select-Object -Unique)) {
        $gpuItems += @{ Value = $n; Text = $n }
    }
} catch { }
$gpuItems += @{ Value = 'warp'; Text = 'WARP (Windows itself, no card: slow)' }
if (-not ($gpuItems | Where-Object { [string]::Equals($_.Value, $gpu, [StringComparison]::OrdinalIgnoreCase) })) {
    $gpuItems += @{ Value = $gpu; Text = "$gpu (from display.ini)" }
}
$gpuBox = New-Choice $gpuItems $gpu $LC 220 $LW `
    "Auto picks the fastest card (on a laptop, the NVIDIA or AMD one, not the processor's own)."
$cursorBox = New-Check 'Hide the mouse pointer when it stops moving' ((Get-Number (Get-Setting $displayIni '' 'hide_cursor')) -ne 0) $L 256 344 `
    'Hides the pointer over the game after 2 seconds without moving it.'
$overlayBox = New-Check 'Show the frame rate in the corner' ((Get-Number (Get-Setting $displayIni '' 'overlay')) -ne 0) $L 282 344 `
    'F1 shows or hides it while playing.'

# Online (right column)
$R = 400; $RC = 524; $RW = 220
[void](New-Text 'Online' $R 56 344 -Bold)
[void](New-Text 'Your name' $R 87 120)
$nameBox = New-TextBox ([string](Get-LineValue $nameFile)) 32 $RC 84 $RW `
    'What other players see. Leave it empty and the game picks one, such as Player1234.'
[void](New-Text 'Lobby' $R 121 120)
$modeBox = New-Choice @(
    @{ Value = 'public'; Text = 'Public lobby' },
    @{ Value = 'private'; Text = 'Private lobby (a code)' },
    @{ Value = 'custom'; Text = 'Your own server' },
    @{ Value = 'offline'; Text = 'Offline' }) $netMode $RC 118 $RW `
    "Public lobby: everyone's rooms in Local Wireless, and Online.`nPrivate lobby: only the players with the same code.`nYour own server: the host:port of a lobby you run (docs\MULTIPLAYER.md)."
[void](New-Text 'Lobby code' $R 155 120)
$codeBox = New-TextBox $lobbyCode 32 $RC 152 $RW 'A word your friends type too, without spaces. Capital letters count: Smash and smash are two lobbies.'
[void](New-Text 'Server' $R 189 120)
$serverBox = New-TextBox $(if ($netMode -eq 'custom') { $lobbyHost } else { '' }) 0 $RC 186 $RW 'host:port, e.g. 192.168.1.20:39420'
$directBox = New-Check 'Direct connections' (-not ((Get-LineValue $directFile) -match '^(?i)off')) $R 222 344 `
    "The players in your rooms connect to you directly, which is faster.`nOff: everything goes through the lobby, and they don't learn your internet address."
$netNote = New-Text '' $R 250 344 34 -Dim
$updateNet = {
    $m = $modeBox.SelectedItem.Value
    $codeBox.Enabled = $m -eq 'private' -or $m -eq 'custom'
    $serverBox.Enabled = $m -eq 'custom'
    $nameBox.Enabled = $m -ne 'offline'
    $directBox.Enabled = $m -ne 'offline'
    $netNote.Text = switch ($m) {
        'public' { "The game joins $PublicLobby." }
        'private' { 'Only the players with the same code (same capitals too) see your rooms.' }
        'custom' { 'Your own lobby server; the code is optional.' }
        'offline' { 'The game does not go online.' }
    }
}
$modeBox.Add_SelectedIndexChanged($updateNet)
& $updateNet

# Controllers
[void](New-Text 'Controllers' $L 332 344 -Bold)
[void](New-Text 'Keyboard plays as' $L 363 124)
$kbItems = @(1..8 | ForEach-Object { @{ Value = "$_"; Text = "Player $_" } }) + @(@{ Value = '0'; Text = 'Nobody (no keyboard)' })
$keyboardBox = New-Choice $kbItems ([string](Get-Number (Get-Setting $padsIni 'general' 'keyboard_player'))) $LC 360 $LW `
    "The keyboard adds to that player's controller."
$backgroundBox = New-Check 'Controllers work while another window is in front' ((Get-Number (Get-Setting $padsIni 'general' 'background')) -ne 0) $R 362 344 `
    "Off: controllers work only while the game's window is in front."
$padsButton = New-Object Windows.Forms.Button
$padsButton.Text = 'Open controllers.ini'
[void](Add-Control $padsButton $L 398 170 30 'Pins, layouts and remaps by hand (the game writes the file at its first start).')
[void](New-Text "Each controller is set up in the game: press Esc, or the controller's Home / PS / Xbox button, and go to Controllers." 196 396 548 40 -Dim)
$padsButton.Add_Click({
    if (Test-Path -LiteralPath $padsIni) { Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$padsIni`"" }
    else { [void][Windows.Forms.MessageBox]::Show($form, "controllers.ini is written at the game's first start.", 'WindowSmash', 'OK', 'Information') }
})

$sep = New-Object Windows.Forms.Label
$sep.BorderStyle = [Windows.Forms.BorderStyle]::Fixed3D
[void](Add-Control $sep $L 448 728 2)

$directStart = New-Check 'Start the game directly next time (hold Shift while starting to see this window again)' (-not (Test-ShowLauncher)) $L 462 728
if (-not $built) {
    $none = New-Text $NotBuilt $L 496 728
    $none.ForeColor = [Drawing.Color]::Firebrick
}

$play = New-Object Windows.Forms.Button
$play.Text = 'Play'
$play.Font = $boldFont
[void](Add-Control $play 488 528 150 32)
$play.Enabled = $built
$close = New-Object Windows.Forms.Button
$close.Text = 'Close'
$close.DialogResult = [Windows.Forms.DialogResult]::Cancel
[void](Add-Control $close 644 528 100 32)
$form.AcceptButton = $play
$form.CancelButton = $close

function Get-WindowValues {
    <# What the window says, file by file: 'file|section|key' = value for the ini keys, and
       'file|' = line for the one-line files. #>
    $onOff = { param($b) if ($b) { '1' } else { '0' } }
    $code = $codeBox.Text -replace '\s', ''
    $server = switch ($modeBox.SelectedItem.Value) {
        'offline' { 'off' }
        'public' { $PublicLobby }
        'private' { "$PublicLobby $code" }
        'custom' { $s = $serverBox.Text.Trim(); if (-not $s) { $s = '127.0.0.1:39420' }; if ($code) { "$s $code" } else { $s } }
    }
    [ordered]@{
        "$displayIni||window" = $windowBox.SelectedItem.Value
        "$displayIni||width" = [string][int]$widthBox.Value
        "$displayIni||height" = [string][int]$heightBox.Value
        "$displayIni||pacing" = $pacingBox.SelectedItem.Value
        "$displayIni||render_scale" = $scaleBox.SelectedItem.Value
        "$displayIni||gpu" = $gpuBox.SelectedItem.Value
        "$displayIni||hide_cursor" = & $onOff $cursorBox.Checked
        "$displayIni||overlay" = & $onOff $overlayBox.Checked
        "$padsIni|general|keyboard_player" = $keyboardBox.SelectedItem.Value
        "$padsIni|general|background" = & $onOff $backgroundBox.Checked
        "$launcherIni||show_launcher" = & $onOff (-not $directStart.Checked)
        "$serverFile|" = $server
        "$nameFile|" = $nameBox.Text.Trim()
        "$directFile|" = if ($directBox.Checked) { 'on' } else { 'off' }
    }
}

function Save-WindowValues($Before, $Now) {
    <# Writes the values changed since the window opened. #>
    foreach ($k in $Now.Keys) {
        $v = $Now[$k]
        if ([string]::Equals([string]$Before[$k], [string]$v)) { continue }
        $file, $section, $key = $k -split '\|', 3
        $leaf = Split-Path -Leaf $file
        if ($DryRun) { Write-Host "would set $leaf`: $(if ($section) { "[$section] " })$(if ($key) { "$key = " })$v"; continue }
        if ($null -eq $key) {
            if ($v) { [void](Set-LineValue $file $v) }
            elseif (Test-Path -LiteralPath $file) {
                # (no name: the game picks one; the name's line goes, the comments stay)
                $t = Read-TextLines $file
                $i = Find-FirstLine $t
                if ($i -ge 0) { $t.Lines.RemoveAt($i); Write-TextLines $file $t }
            }
            continue
        }
        if ($file -eq $launcherIni -and -not (Test-Path -LiteralPath $file)) {
            [IO.File]::WriteAllText($file, "; launcher.ini: the window Play SSBU.bat opens (tools\launcher.ps1).`r`n" +
                "; show_launcher = 0: start the game directly; hold Shift while starting to see the window.`r`n", [Text.Encoding]::ASCII)
        }
        [void](Set-IniValue $file $section $key $v)
    }
}

$before = Get-WindowValues
$script:played = $null
$play.Add_Click({
    if ($modeBox.SelectedItem.Value -eq 'private' -and -not ($codeBox.Text -replace '\s', '')) {
        [void][Windows.Forms.MessageBox]::Show($form, 'Type a lobby code: a word your friends type too.', 'WindowSmash', 'OK', 'Warning')
        [void]$codeBox.Focus()
        return
    }
    if ($modeBox.SelectedItem.Value -eq 'custom' -and $serverBox.Text.Trim() -and $serverBox.Text.Trim() -notmatch '^\S+:\d+$') {
        [void][Windows.Forms.MessageBox]::Show($form, 'Your own server is host:port, e.g. 192.168.1.20:39420.', 'WindowSmash', 'OK', 'Warning')
        return
    }
    try { Save-WindowValues $before (Get-WindowValues) } catch {
        [void][Windows.Forms.MessageBox]::Show($form, "The settings could not be saved: $($_.Exception.Message)", 'WindowSmash', 'OK', 'Error')
        return
    }
    $script:played = $true
    $form.Close()
})

[void]$form.ShowDialog()
} catch {
    if ($script:played) { throw }
    Write-Host "The launcher window cannot open here ($($_.Exception.Message))."
    exit 2
}
if ($script:played) {
    Start-Game
}
exit 0
