param(
    [string]$SavePath,
    [string]$Root,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    # The WindowSmash folder (-Root: tests). The save layout is 13.0.5's, known by the main
    # module's build id (the same table as tools/unlock_all.py).
    $Folder = if ($Root) { $Root } else { Split-Path -Parent $PSScriptRoot }
    # build id = game version, save size, FLAGS, RECORDS
    $knownLayouts = @{
        '21450C647B8C5940ED5B68218EA128FD3C5D597D' = @('13.0.5', 0x5B4AF8, 0x53FF40, 0x54FEA8)
    }
    $knownVersions = ($knownLayouts.Values | ForEach-Object { $_[0] } | Sort-Object) -join ', '
    $mainPath = Join-Path $Folder 'dump\exefs\main'
    if (-not (Test-Path -LiteralPath $mainPath -PathType Leaf)) { throw "No game in $Folder (dump\exefs\main is missing)." }
    $stream = [System.IO.File]::OpenRead($mainPath)
    try { $head = New-Object byte[] 0x60; [void]$stream.Read($head, 0, 0x60) } finally { $stream.Close() }
    $buildId = (($head[0x40..0x5F] | ForEach-Object { $_.ToString('X2') }) -join '').TrimEnd('0')
    if (-not $knownLayouts.ContainsKey($buildId)) {
        $versionFile = Join-Path $Folder 'version.txt'
        $version = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -TotalCount 1) } else { 'unknown' }
        throw "That is game version $version; this tool knows the save layout of $knownVersions only, so it changes nothing."
    }
    $version, $saveSize, $flagsOffset, $recordsOffset = $knownLayouts[$buildId]
    Write-Host ('Game version ' + $version + ': ' + $Folder)
    if (-not $SavePath) {
        $SavePath = Join-Path $Folder 'save\0123456789abcdeffedcba9876543210\save_data\system_data.bin'
    }
    if (@(Get-Process -Name ssbu,ssbu_new -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Close every SSBU game window before unlocking fighters, then run this again.'
    }
    if (-not (Test-Path -LiteralPath $SavePath -PathType Leaf)) {
        throw 'No save found. Start the updated game once, reach the main menu, close it, then run launchers\Unlock All Fighters.bat again.'
    }
    $SavePath = (Resolve-Path -LiteralPath $SavePath).Path
    $originalInfo = Get-Item -LiteralPath $SavePath
    $originalWriteTime = $originalInfo.LastWriteTimeUtc
    [byte[]]$data = [System.IO.File]::ReadAllBytes($SavePath)
    $starters = @(0, 1, 2, 3, 5, 6, 7, 8)
    $fighterNames = @(
        'Mario', 'Donkey Kong', 'Link', 'Samus', 'Dark Samus', 'Yoshi', 'Kirby', 'Fox', 'Pikachu', 'Luigi',
        'Ness', 'Captain Falcon', 'Jigglypuff', 'Peach', 'Daisy', 'Bowser', 'Ice Climbers', 'Sheik', 'Zelda',
        'Dr. Mario', 'Pichu', 'Falco', 'Marth', 'Lucina', 'Young Link', 'Ganondorf', 'Mewtwo', 'Roy', 'Chrom',
        'Mr. Game & Watch', 'Meta Knight', 'Pit', 'Dark Pit', 'Zero Suit Samus', 'Wario', 'Snake', 'Ike',
        'Pokemon Trainer', 'Diddy Kong', 'Lucas', 'Sonic', 'King Dedede', 'Olimar', 'Lucario', 'R.O.B.',
        'Toon Link', 'Wolf', 'Villager', 'Mega Man', 'Wii Fit Trainer', 'Rosalina & Luma', 'Little Mac',
        'Greninja', 'Palutena', 'Pac-Man', 'Robin', 'Shulk', 'Bowser Jr.', 'Duck Hunt', 'Ryu', 'Ken', 'Cloud',
        'Corrin', 'Bayonetta', 'Richter', 'Inkling', 'Ridley', 'King K. Rool', 'Simon', 'Isabelle',
        'Incineroar', 'Mii Brawler', 'Mii Swordfighter', 'Mii Gunner'
    )
    [uint64]$highBit = [uint64]::Parse('8000000000000000', [Globalization.NumberStyles]::HexNumber)
    if ($data.Length -ne $saveSize -or [BitConverter]::ToUInt32($data, 0) -ne $saveSize) {
        throw "Unsupported save size/header for game version $version. The save has not been changed."
    }
    $order = 0
    for ($n = 0; $n -lt $fighterNames.Count; $n++) {
        $offset = $flagsOffset + 8 * $n
        if ($data[$offset] -gt 1 -or $data[$offset + 5] -ne 0 -or $data[$offset + 6] -ne 0 -or $data[$offset + 7] -ne 0) {
            throw 'Unsupported fighter table. The save has not been changed.'
        }
        if ($starters -contains $n -and $data[$offset] -ne 1) {
            throw 'Starter fighter flags are invalid. The save has not been changed.'
        }
        [uint64]$oldRecord = [BitConverter]::ToUInt64($data, $recordsOffset + 8 * $n)
        if ($oldRecord -ne 0 -and ($oldRecord -band $highBit) -eq 0) {
            throw 'Unsupported milestone table. The save has not been changed.'
        }
        $order = [Math]::Max($order, [int]$data[$offset + 4])
    }
    $now = Get-Date
    [uint64]$record = $highBit -bor ([uint64]3 -shl 48) -bor ([uint64]$now.Year) -bor ([uint64]$now.Month -shl 12) -bor ([uint64]$now.Day -shl 16) -bor ([uint64]$now.Hour -shl 21) -bor ([uint64]$now.Minute -shl 26) -bor ([uint64]$now.Second -shl 32)
    $added = [System.Collections.Generic.List[string]]::new()
    for ($n = 0; $n -lt $fighterNames.Count; $n++) {
        $offset = $flagsOffset + 8 * $n
        if ($data[$offset] -eq 1) { continue }
        if ($data[$offset + 4] -eq 0) {
            $order++
            if ($order -gt 255) { throw 'Fighter encounter order exceeds the supported range.' }
            $data[$offset + 4] = [byte]$order
        }
        $data[$offset] = 1
        $data[$offset + 1] = 0
        $data[$offset + 2] = 0
        $data[$offset + 3] = 0
        $recordOffset = $recordsOffset + 8 * $n
        if ([BitConverter]::ToUInt64($data, $recordOffset) -eq 0) {
            [BitConverter]::GetBytes($record).CopyTo($data, $recordOffset)
        }
        $added.Add($fighterNames[$n])
    }
    if ($added.Count -eq 0) {
        Write-Host 'Every base-game fighter is already unlocked. DLC fighters are enabled by the update.'
        exit 0
    }
    Write-Host ('Fighters to unlock: ' + $added.Count)
    Write-Host ($added -join ', ')
    if ($DryRun) {
        Write-Host 'Dry run: no save or backup files were written.'
        exit 0
    }
    $currentInfo = Get-Item -LiteralPath $SavePath
    if ($currentInfo.Length -ne $data.Length -or $currentInfo.LastWriteTimeUtc -ne $originalWriteTime) {
        throw 'The save changed while the tool was running. Close the game and retry.'
    }
    $saveRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $SavePath)))
    $backupFolder = Join-Path $saveRoot 'save_backups'
    [void][System.IO.Directory]::CreateDirectory($backupFolder)
    $token = [guid]::NewGuid().ToString('N')
    $backup = Join-Path $backupFolder ('system_data.bin.before-unlock-' + $now.ToString('yyyyMMdd_HHmmss_fff') + '-' + $token)
    $temporary = $SavePath + '.unlock-' + $token + '.tmp'
    [System.IO.File]::WriteAllBytes($temporary, $data)
    [System.IO.File]::Replace($temporary, $SavePath, $backup)
    Write-Host 'Unlocked. Your previous save is backed up at:'
    Write-Host $backup
    Write-Host 'Start the game. On the milestone notices, press the in-game X button to skip them.'
    Write-Host 'Keyboard: C is the in-game X button. Create Mii Fighters under Games and More if needed.'
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
