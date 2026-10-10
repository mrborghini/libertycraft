param(
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

function Get-SteamRoots {
    $roots = [System.Collections.Generic.List[string]]::new()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\Software\WOW6432Node\Valve\Steam', 'HKLM:\Software\Valve\Steam')) {
        try {
            $item = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
            foreach ($value in @($item.SteamPath, $item.InstallPath)) {
                if ($value -and (Test-Path -LiteralPath $value)) { $roots.Add((Join-Path $value '.')) }
            }
        } catch { }
    }
    foreach ($candidate in @((Join-Path ${env:ProgramFiles(x86)} 'Steam'), (Join-Path $env:ProgramFiles 'Steam'))) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { $roots.Add($candidate) }
    }
    return @($roots | Select-Object -Unique)
}

function Get-SteamLibraries {
    $libraries = [System.Collections.Generic.List[string]]::new()
    foreach ($root in Get-SteamRoots) {
        $libraries.Add($root)
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in Get-Content -LiteralPath $vdf) {
                if ($line -match '^\s*"(?:path|\d+)"\s+"([^"]+)"') {
                    $path = $Matches[1].Replace('\\', '\')
                    if (Test-Path -LiteralPath $path) { $libraries.Add($path) }
                }
            }
        }
    }
    return @($libraries | Select-Object -Unique)
}

function Resolve-GameDirectory([string]$Path) {
    if (Test-Path -LiteralPath (Join-Path $Path 'GTAIV.exe')) { return (Resolve-Path -LiteralPath $Path).Path }
    if (Test-Path -LiteralPath (Join-Path $Path 'GTAIV\GTAIV.exe')) { return (Resolve-Path -LiteralPath (Join-Path $Path 'GTAIV')).Path }
    throw "The selected folder does not contain GTAIV.exe: $Path"
}

function Find-GameDirectory([string]$Path = '') {
    if ($Path) { return Resolve-GameDirectory $Path }

    $candidates = [System.Collections.Generic.List[string]]::new()
    foreach ($library in Get-SteamLibraries) {
        $apps = Join-Path $library 'steamapps'
        $manifest = Join-Path $apps 'appmanifest_12210.acf'
        $installName = 'Grand Theft Auto IV'
        if (Test-Path -LiteralPath $manifest) {
            $match = Select-String -LiteralPath $manifest -Pattern '"installdir"\s+"([^"]+)"' | Select-Object -First 1
            if ($match -and $match.Matches[0].Groups[1].Value) { $installName = $match.Matches[0].Groups[1].Value }
        }
        $common = Join-Path $apps 'common'
        foreach ($candidate in @(
            (Join-Path $common "$installName\GTAIV"),
            (Join-Path $common $installName),
            (Join-Path $common 'Grand Theft Auto IV\GTAIV'),
            (Join-Path $common 'Grand Theft Auto IV')
        )) { $candidates.Add($candidate) }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'GTAIV.exe')) { return (Resolve-Path -LiteralPath $candidate).Path }
    }

    $typed = Read-Host 'Steam detection did not find GTA IV. Enter the folder containing GTAIV.exe (or press Enter to cancel)'
    if (-not $typed) { throw 'GTA IV folder was not selected.' }
    return Resolve-GameDirectory $typed
}

try {
    $game = Find-GameDirectory $GameDir
    $ini = Join-Path $game 'ZolikaPatch.ini'
    if (-not (Test-Path -LiteralPath (Join-Path $game 'ZolikaPatch.asi'))) {
        throw "ZolikaPatch.asi was not found in $game. Install LibertyCraft first, then run this repair script."
    }
    if (-not (Test-Path -LiteralPath $ini)) { throw "ZolikaPatch.ini was not found in $game." }
    if (Get-Process -Name 'GTAIV', 'PlayGTAIV' -ErrorAction SilentlyContinue) {
        throw 'Quit GTA IV before running this repair script.'
    }

    $bytes = [IO.File]::ReadAllBytes($ini)
    $encoding = [Text.Encoding]::Default
    $bom = [byte[]]@()
    $offset = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $encoding = [Text.UTF8Encoding]::new($false, $true)
        $bom = [byte[]]@(0xEF, 0xBB, 0xBF)
        $offset = 3
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $encoding = [Text.UnicodeEncoding]::new($false, $false, $true)
        $bom = [byte[]]@(0xFF, 0xFE)
        $offset = 2
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
        $encoding = [Text.UnicodeEncoding]::new($true, $false, $true)
        $bom = [byte[]]@(0xFE, 0xFF)
        $offset = 2
    } else {
        try {
            $encoding = [Text.UTF8Encoding]::new($false, $true)
            $null = $encoding.GetString($bytes)
        } catch {
            $encoding = [Text.Encoding]::Default
        }
    }

    $text = $encoding.GetString($bytes, $offset, $bytes.Length - $offset)
    $newline = if ($text.Contains("`r`n")) { "`r`n" } elseif ($text.Contains("`r")) { "`r" } else { "`n" }
    $lines = [regex]::Split($text, '\r\n|\r|\n')
    $inOptions = $false
    $found = 0
    $changed = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match '^\s*\[([^\]]+)\]\s*$') {
            $inOptions = $Matches[1] -ieq 'Options'
            continue
        }
        if (-not $inOptions -or $lines[$i] -notmatch '^\s*SkipLauncher\s*=') { continue }
        if ($lines[$i] -notmatch '^(\s*SkipLauncher\s*=\s*)([01])(\s*(?:[;#].*)?)$') {
            throw 'The SkipLauncher value in [Options] is not 0 or 1. ZolikaPatch.ini was not changed.'
        }
        $found++
        if ($Matches[2] -eq '1') {
            $lines[$i] = [regex]::Replace($lines[$i], '^(\s*SkipLauncher\s*=\s*)[01](\s*(?:[;#].*)?)$', '${1}0${2}', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
            $changed = $true
        }
    }
    if (-not $found) { throw 'The SkipLauncher option was not found under [Options]. ZolikaPatch.ini was not changed.' }
    if (-not $changed) {
        Write-Host "ZolikaPatch is already configured safely in $game." -ForegroundColor Green
        exit 0
    }

    $backupDir = Join-Path $game '_libertycraft_backup'
    $backup = Join-Path $backupDir 'ZolikaPatch.ini.before-zolika-fix'
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $ini -Destination $backup
        Write-Host "Saved the previous settings to $backup"
    } else {
        Write-Host "Keeping the existing settings backup at $backup"
    }

    $newText = [string]::Join($newline, $lines)
    $content = $encoding.GetBytes($newText)
    $updated = New-Object byte[] ($bom.Length + $content.Length)
    if ($bom.Length) { [Array]::Copy($bom, 0, $updated, 0, $bom.Length) }
    if ($content.Length) { [Array]::Copy($content, 0, $updated, $bom.Length, $content.Length) }
    [IO.File]::WriteAllBytes($ini, $updated)

    Write-Host "Set ZolikaPatch [Options] SkipLauncher=0 in $ini." -ForegroundColor Green
    Write-Host 'Run launch-windows.bat again to start LibertyCraft.'
} catch {
    Write-Host "`nZolika fix error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
