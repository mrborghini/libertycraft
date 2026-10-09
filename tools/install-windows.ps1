param(
    [switch]$Uninstall,
    [switch]$NoMinecraft,
    [string]$GameDir,
    [string]$PrismDir
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

$script:InstallRoot = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $script:InstallRoot 'dist'))) {
    $script:InstallRoot = Split-Path -Parent $PSScriptRoot
}
$script:BackupName = '_libertycraft_backup'
$script:BaseAssetsUrl = 'https://github.com/gillian-guide/GTAIVFullDowngradeAssets/releases/download/Base/BaseAssets.zip'
$script:BaseAssetsSha256 = 'dff2ad5da752157c466f7d5721a19132ac42a41298f959b4f89243e31c95150b'
$script:DowngradeExecutableLength = 15628696
$script:DowngradeExecutableVersion = '1.0.8.0'
$script:FabricApiUrl = 'https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar'
$script:FabricApiSha512 = 'ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d'
$script:FabricApiJarName = 'fabric-api-0.161.0+26.3.jar'
$script:Manifest = @{}
$script:ManifestOrder = [System.Collections.Generic.List[string]]::new()
$script:Game = $null
$script:Cache = Join-Path $env:LOCALAPPDATA 'LibertyCraft\cache'
$script:Stage = Join-Path ([IO.Path]::GetTempPath()) ('libertycraft-' + [guid]::NewGuid().ToString('N'))

function Write-Step([string]$Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Write-Info([string]$Message) { Write-Host "    $Message" }
function Stop-Install([string]$Message) { throw $Message }

function Get-ExecutableVersion([string]$Path) {
    $version = (Get-Item -LiteralPath $Path).VersionInfo
    return '{0}.{1}.{2}.{3}' -f $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart, $version.FilePrivatePart
}

function Assert-ExecutableVersionAndSize([string]$Path, [string]$Description, [long]$ExpectedLength, [string]$ExpectedVersion) {
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -ne $ExpectedLength) {
        Stop-Install "$Description has size $($file.Length) bytes; expected $ExpectedLength bytes."
    }
    $version = Get-ExecutableVersion $Path
    if ($version -ne $ExpectedVersion) {
        $reportedVersion = $file.VersionInfo.FileVersion
        Stop-Install "$Description has fixed file version '$version' (Windows reports '$reportedVersion'); expected $ExpectedVersion."
    }
}

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

function Find-GameDirectory([string]$Path = '') {
    if ($Path) {
        if (Test-Path -LiteralPath (Join-Path $Path 'GTAIV.exe')) { return (Resolve-Path -LiteralPath $Path).Path }
        if (Test-Path -LiteralPath (Join-Path $Path 'GTAIV\GTAIV.exe')) { return (Resolve-Path -LiteralPath (Join-Path $Path 'GTAIV')).Path }
        Stop-Install "The selected game folder does not contain GTAIV.exe: $Path"
    }
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
    if (-not $typed) { Stop-Install 'GTA IV folder was not selected.' }
    return (Find-GameDirectory $typed)
}

function Find-PrismDirectory {
    if ($PrismDir) {
        if (Test-Path -LiteralPath $PrismDir) { return (Resolve-Path -LiteralPath $PrismDir).Path }
        Stop-Install "The selected Prism data folder does not exist: $PrismDir"
    }
    foreach ($candidate in @(
        (Join-Path $env:APPDATA 'PrismLauncher'),
        (Join-Path $env:LOCALAPPDATA 'PrismLauncher'),
        (Join-Path $env:APPDATA 'Prism Launcher')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    $typed = Read-Host 'Prism Launcher was not found. Enter its data folder (or press Enter to skip Minecraft setup)'
    if (-not $typed) { return $null }
    if (-not (Test-Path -LiteralPath $typed)) { Stop-Install "The selected Prism data folder does not exist: $typed" }
    return (Resolve-Path -LiteralPath $typed).Path
}

function Read-Manifest {
    $manifestFile = Join-Path (Join-Path $script:Game $script:BackupName) 'manifest.txt'
    if (-not (Test-Path -LiteralPath $manifestFile)) { return }
    foreach ($line in Get-Content -LiteralPath $manifestFile) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $parts = $line -split "`t", 2
        if ($parts.Count -ne 2 -or $parts[0] -notin @('added', 'replaced', 'removed', 'mkdir')) { continue }
        if (-not $script:Manifest.ContainsKey($parts[1])) { $script:ManifestOrder.Add($parts[1]) }
        $script:Manifest[$parts[1]] = $parts[0]
    }
}

function Write-Manifest {
    $backup = Join-Path $script:Game $script:BackupName
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    $manifestFile = Join-Path $backup 'manifest.txt'
    $rows = @(
        '# LibertyCraft install manifest. Original files are stored in this folder.',
        '# added files are deleted on uninstall; replaced and removed files are restored.'
    )
    foreach ($relative in $script:ManifestOrder) {
        if ($script:Manifest.ContainsKey($relative)) { $rows += "$($script:Manifest[$relative])`t$relative" }
    }
    [IO.File]::WriteAllLines($manifestFile, [string[]]$rows, [Text.UTF8Encoding]::new($false))
}

function Set-ManifestEntry([string]$Action, [string]$Relative) {
    if (-not $script:Manifest.ContainsKey($Relative)) { $script:ManifestOrder.Add($Relative) }
    $script:Manifest[$Relative] = $Action
    Write-Manifest
}

function Remove-ManifestEntry([string]$Relative) {
    $script:Manifest.Remove($Relative) | Out-Null
    Write-Manifest
}

function Ensure-GameDirectory([string]$Relative) {
    if (-not $Relative -or $Relative -eq '.') { return }
    $current = $script:Game
    foreach ($part in ($Relative -split '[\\/]')) {
        if (-not $part) { continue }
        $current = Join-Path $current $part
        if (Test-Path -LiteralPath $current -PathType Container) { continue }
        if (Test-Path -LiteralPath $current) { Stop-Install "$current exists but is not a directory." }
        New-Item -ItemType Directory -Path $current | Out-Null
        $relativePath = $current.Substring($script:Game.TrimEnd('\').Length).TrimStart('\')
        Set-ManifestEntry 'mkdir' $relativePath
    }
}

function Install-File([string]$Source, [string]$Relative, [switch]$KeepExisting) {
    $destination = Join-Path $script:Game $Relative
    $state = $script:Manifest[$Relative]
    if ($KeepExisting -and $state -and (Test-Path -LiteralPath $destination)) { return }
    if ((Test-Path -LiteralPath $destination) -and (Test-Path -LiteralPath $Source) -and
        ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash)) { return }
    Ensure-GameDirectory (Split-Path -Parent $Relative)
    if (-not $state) {
        if (Test-Path -LiteralPath $destination) {
            $backup = Join-Path (Join-Path $script:Game $script:BackupName) $Relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
            if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $destination -Destination $backup }
            Set-ManifestEntry 'replaced' $Relative
        } else { Set-ManifestEntry 'added' $Relative }
    }
    Copy-Item -LiteralPath $Source -Destination $destination -Force
    Write-Info "Installed $Relative"
}

function Remove-GameFile([string]$Relative) {
    $destination = Join-Path $script:Game $Relative
    if (-not (Test-Path -LiteralPath $destination)) { return }
    $state = $script:Manifest[$Relative]
    if ($state -eq 'added') { Remove-ManifestEntry $Relative }
    elseif (-not $state) {
        $backup = Join-Path (Join-Path $script:Game $script:BackupName) $Relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $destination -Destination $backup }
        Set-ManifestEntry 'removed' $Relative
    }
    Remove-Item -LiteralPath $destination -Force
}

function Restore-GameFiles {
    Write-Step "Restoring files in $script:Game"
    $backupRoot = Join-Path $script:Game $script:BackupName
    $ordered = @($script:ManifestOrder.ToArray())
    [array]::Reverse($ordered)
    foreach ($relative in $ordered) {
        $destination = Join-Path $script:Game $relative
        switch ($script:Manifest[$relative]) {
            'added' { if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force } }
            'mkdir' { if (Test-Path -LiteralPath $destination -PathType Container) { Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue } }
            { $_ -in @('replaced', 'removed') } {
                $backup = Join-Path $backupRoot $relative
                if (Test-Path -LiteralPath $backup) {
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
                    Copy-Item -LiteralPath $backup -Destination $destination -Force
                }
            }
        }
        Write-Info "Restored $relative"
    }
    Remove-Item -LiteralPath (Join-Path $backupRoot 'manifest.txt') -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $backupRoot) {
        $remaining = Get-ChildItem -LiteralPath $backupRoot -Force -Recurse
        if (-not $remaining) { Remove-Item -LiteralPath $backupRoot -Force }
    }
}

function Get-Download([string]$Url, [string]$Destination, [string]$ExpectedHash = '', [string]$HashAlgorithm = 'SHA256', [switch]$Refresh) {
    if ($Refresh -and (Test-Path -LiteralPath $Destination)) { Remove-Item -LiteralPath $Destination -Force }
    if (-not (Test-Path -LiteralPath $Destination)) {
        Write-Info "Downloading $([IO.Path]::GetFileName($Destination))"
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    }
    if ($ExpectedHash) {
        $actual = (Get-FileHash -LiteralPath $Destination -Algorithm $HashAlgorithm).Hash.ToLowerInvariant()
        if ($actual -ne $ExpectedHash.ToLowerInvariant()) {
            Remove-Item -LiteralPath $Destination -Force
            Stop-Install "$HashAlgorithm verification failed for $Destination"
        }
    }
}

function Expand-ZipSelection([string]$ZipPath, [string]$Destination, [string[]]$Prefixes, [string[]]$Exact = @()) {
    $archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $found = @{}
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if (-not $name -or $name.EndsWith('/')) { continue }
            $selected = $false
            foreach ($prefix in $Prefixes) { if ($name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $selected = $true } }
            foreach ($exactName in $Exact) { if ($name.Equals($exactName, [StringComparison]::OrdinalIgnoreCase)) { $selected = $true } }
            if (-not $selected) { continue }
            $relative = $name -replace '^[A-Za-z]:', ''
            if ($relative.StartsWith('/') -or $relative.Contains(':') -or $relative.Split('/') -contains '..') { Stop-Install "Unsafe archive entry: $name" }
            $target = Join-Path $Destination ($relative -replace '/', '\')
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            $found[$name.ToLowerInvariant()] = $true
        }
        foreach ($name in $Exact) {
            if (-not $found.ContainsKey($name.ToLowerInvariant())) { Stop-Install "$ZipPath does not contain required entry $name" }
        }
    } finally { $archive.Dispose() }
}

function Install-Tree([string]$SourceRoot, [string]$RelativeRoot = '', [switch]$KeepIni) {
    foreach ($file in Get-ChildItem -LiteralPath $SourceRoot -File -Recurse) {
        $tail = $file.FullName.Substring($SourceRoot.TrimEnd('\').Length).TrimStart('\')
        $relative = if ($RelativeRoot) { Join-Path $RelativeRoot $tail } else { $tail }
        $keep = $KeepIni -and $relative.EndsWith('.ini', [StringComparison]::OrdinalIgnoreCase)
        Install-File $file.FullName $relative -KeepExisting:$keep
    }
}

function Set-IniOptions([string]$Path, [string[]]$Pairs) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $Path) { foreach ($line in Get-Content -LiteralPath $Path) { $lines.Add($line) } }
    $sectionStart = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[([^\]]+)\]') {
            if ($Matches[1] -ieq 'Options') { $sectionStart = $i; break }
        }
    }
    if ($sectionStart -lt 0) { return }
    $sectionEnd = $lines.Count
    for ($i = $sectionStart + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[') { $sectionEnd = $i; break }
    }
    foreach ($pair in $Pairs) {
        $key, $value = $pair -split '=', 2
        for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { $lines[$i] = "$key=$value"; break }
        }
    }
    [IO.File]::WriteAllLines($Path, [string[]]$lines.ToArray(), [Text.UTF8Encoding]::new($false))
}

function Setup-Prism([string]$PrismData) {
    if (-not $PrismData) { Write-Warning 'Prism setup skipped. Install Prism Launcher, sign in, then run this installer again.'; return }
    $instancesDir = Join-Path $PrismData 'instances'
    $config = Join-Path $PrismData 'prismlauncher.cfg'
    if (Test-Path -LiteralPath $config) {
        $instanceLine = Select-String -LiteralPath $config -Pattern '^InstanceDir=(.*)$' | Select-Object -First 1
        if ($instanceLine) {
            $configured = $instanceLine.Matches[0].Groups[1].Value
            if ([IO.Path]::IsPathRooted($configured)) { $instancesDir = $configured } else { $instancesDir = Join-Path $PrismData $configured }
        }
    }
    $instance = Join-Path $instancesDir 'LibertyCraft'
    $minecraft = Join-Path $instance 'minecraft'
    $mods = Join-Path $minecraft 'mods'
    New-Item -ItemType Directory -Force -Path $mods | Out-Null
    $instanceCfg = Join-Path $instance 'instance.cfg'
    $cfg = @{}
    if (Test-Path -LiteralPath $instanceCfg) {
        foreach ($line in Get-Content -LiteralPath $instanceCfg) {
            if ($line -match '^([^=]+)=(.*)$') { $cfg[$Matches[1]] = $Matches[2] }
        }
    }
    $cfg['InstanceType'] = 'OneSix'
    $cfg['OverrideJavaArgs'] = 'true'
    $cfg['JvmArgs'] = '--enable-native-access=ALL-UNNAMED -Dlibertycraft.startHidden=true'
    $cfg['OverrideMemory'] = 'true'
    $cfg['ConfigVersion'] = '1.3'
    $cfg['name'] = 'LibertyCraft'
    $cfg['MinMemAlloc'] = '1024'
    $cfg['MaxMemAlloc'] = '8192'
    $cfg['OverrideConsole'] = 'true'
    $cfg['ShowConsole'] = 'false'
    $cfg['AutoCloseConsole'] = 'false'
    $cfg['ShowConsoleOnError'] = 'true'
    $cfgLines = @($cfg.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" })
    [IO.File]::WriteAllLines($instanceCfg, $cfgLines, [Text.UTF8Encoding]::new($false))
    $pack = @{
        components = @(
            @{ cachedName = 'LWJGL 3'; cachedVersion = '3.4.3'; cachedVolatile = $true; dependencyOnly = $true; uid = 'org.lwjgl3'; version = '3.4.3' },
            @{ cachedName = 'Minecraft'; cachedRequires = @(@{ suggests = '3.4.3'; uid = 'org.lwjgl3' }); cachedVersion = '26.3'; important = $true; uid = 'net.minecraft'; version = '26.3' },
            @{ cachedName = 'Intermediary Mappings'; cachedRequires = @(@{ equals = '26.3'; uid = 'net.minecraft' }); cachedVersion = '26.3'; cachedVolatile = $true; dependencyOnly = $true; uid = 'net.fabricmc.intermediary'; version = '26.3' },
            @{ cachedName = 'Fabric Loader'; cachedRequires = @(@{ uid = 'net.fabricmc.intermediary' }); cachedVersion = '0.19.5'; uid = 'net.fabricmc.fabric-loader'; version = '0.19.5' }
        )
        formatVersion = 1
    }
    [IO.File]::WriteAllText((Join-Path $instance 'mmc-pack.json'), ($pack | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $api = Join-Path $script:Stage 'fabric-api.jar'
    Get-Download $script:FabricApiUrl $api $script:FabricApiSha512 -HashAlgorithm 'SHA512'
    Get-ChildItem -LiteralPath $mods -Filter 'fabric-api-*.jar' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    Copy-Item -LiteralPath $api -Destination (Join-Path $mods $script:FabricApiJarName) -Force
    $mod = Join-Path $script:InstallRoot 'dist\mods'
    $jar = Get-ChildItem -LiteralPath $mod -Filter 'libertycraft-*.jar' -File | Select-Object -First 1
    if (-not $jar) { Stop-Install "LibertyCraft Fabric jar was not found in $mod" }
    Get-ChildItem -LiteralPath $mods -Filter 'libertycraft-*.jar' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    Copy-Item -LiteralPath $jar.FullName -Destination $mods -Force
    Write-Info "Configured Prism instance at $instance"
}

function Invoke-Install {
    $script:Game = Find-GameDirectory $GameDir
    $version = Get-ExecutableVersion (Join-Path $script:Game 'GTAIV.exe')
    if ($version -notmatch '^1\.2\.' -and $version -ne '1.0.8.0') {
        Write-Warning "GTAIV.exe reports version '$version'. The downgrade is intended for Complete Edition 1.2.x or 1.0.8.0."
        if ((Read-Host 'Continue anyway? [y/N]') -notmatch '^(y|yes)$') { Stop-Install 'Cancelled.' }
    }
    if (Get-Process -Name 'GTAIV', 'PlayGTAIV' -ErrorAction SilentlyContinue) { Stop-Install 'Quit GTA IV before installing LibertyCraft.' }
    Read-Manifest
    if (-not (Test-Path -LiteralPath (Join-Path $script:InstallRoot 'dist\plugins\LibertyCraft.asi'))) { Stop-Install 'Release package is missing dist\plugins\LibertyCraft.asi.' }
    if (-not (Get-ChildItem -LiteralPath (Join-Path $script:InstallRoot 'dist\mods') -Filter 'libertycraft-*.jar' -File -ErrorAction SilentlyContinue)) { Stop-Install 'Release package is missing the Fabric mod jar.' }
    New-Item -ItemType Directory -Force -Path $script:Cache, $script:Stage | Out-Null

    Write-Step "Preparing GTA IV in $script:Game"
    $base = Join-Path $script:Cache 'BaseAssets.zip'
    $legacy = Join-Path $script:Cache 'GTAIV.EFLC.FusionFixLegacyAddon.zip'
    $fusion = Join-Path $script:Cache 'GTAIV.EFLC.FusionFix.zip'
    Get-Download $script:BaseAssetsUrl $base $script:BaseAssetsSha256
    Get-Download 'https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/releases/latest/download/GTAIV.EFLC.FusionFixLegacyAddon.zip' $legacy '' -Refresh
    Get-Download 'https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/releases/latest/download/GTAIV.EFLC.FusionFix.zip' $fusion '' -Refresh

    $baseDir = Join-Path $script:Stage 'base'
    $legacyDir = Join-Path $script:Stage 'legacy'
    $fusionDir = Join-Path $script:Stage 'fusion'
    Expand-ZipSelection $base $baseDir @('Shared/', 'ZolikaPatch/') @('1080/GTAIV.exe')
    Expand-ZipSelection $legacy $legacyDir @() @('xlive.dll')
    Expand-ZipSelection $fusion $fusionDir @('plugins/', 'update/') @('plugins/GTAIV.EFLC.FusionFix.asi', 'update/update.txt')
    $exe = Join-Path $baseDir '1080\GTAIV.exe'
    Assert-ExecutableVersionAndSize $exe 'Downloaded downgrade executable' $script:DowngradeExecutableLength $script:DowngradeExecutableVersion

    $otherPlugins = @(
        Get-ChildItem -LiteralPath $script:Game -Filter '*.asi' -File -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath (Join-Path $script:Game 'plugins') -Filter '*.asi' -File -ErrorAction SilentlyContinue
    ) | Where-Object { $_.Name -notin @('LibertyCraft.asi', 'GTAIV.EFLC.FusionFix.asi', 'ZolikaPatch.asi') }
    if ($otherPlugins) { Write-Warning "Other ASI plugins are present and may conflict: $($otherPlugins.Name -join ', ')" }
    if ((Read-Host "This will modify $script:Game and keep backups in $script:BackupName. Continue? [y/N]") -notmatch '^(y|yes)$') {
        Stop-Install 'Cancelled before changing the game folder.'
    }

    Remove-GameFile 'dinput8.dll'
    Remove-GameFile 'dsound.dll'
    Remove-GameFile 'plugins\XLivelessAddon.asi'
    Remove-GameFile 'plugins\XLivelessAddon.ini'
    Install-File $exe 'GTAIV.exe'
    Install-Tree (Join-Path $baseDir 'Shared')
    Install-File (Join-Path $baseDir 'ZolikaPatch\ZolikaPatch.asi') 'ZolikaPatch.asi'
    Install-File (Join-Path $baseDir 'ZolikaPatch\ZolikaPatch.ini') 'ZolikaPatch.ini' -KeepExisting
    $zolikaIni = Join-Path $script:Game 'ZolikaPatch.ini'
    Set-IniOptions $zolikaIni @('MiscFixes=0', 'BikeFeetFix=0', 'BikePhoneAnimsFix=0', 'BorderlessWindowed=0', 'BuildingAlphaFix=0', 'BuildingDynamicShadows=0', 'CarDynamicShadowFix=0', 'CarPartsShadowFix=0', 'CutsceneFixes=0', 'DoNotPauseOnMinimize=0', 'DualVehicleHeadlights=0', 'EmissiveLerpFix=0', 'EpisodicVehicleSupport=0', 'EpisodicWeaponSupport=0', 'ForceCarHeadlightShadows=0', 'ForceDynamicShadowsEverywhere=0', 'ForceShadowsOnObjects=0', 'HighFPSBikePhysicsFix=0', 'HighFPSSpeedupFix=0', 'HighQualityReflections=0', 'ImprovedShaderStreaming=0', 'MouseFix=0', 'NewMemorySystem=0', 'NoLiveryLimit=0', 'OutOfCommissionFix=0', 'PoliceEpisodicWeaponSupport=0', 'RemoveBoundingBoxCulling=0', 'ReversingLightFix=0', 'SkipIntro=0', 'SkipMenu=0')
    Install-File (Join-Path $legacyDir 'xlive.dll') 'xlive.dll'
    Install-Tree (Join-Path $fusionDir 'plugins') 'plugins' -KeepIni
    Install-Tree (Join-Path $fusionDir 'update') 'update'
    Install-File (Join-Path $script:InstallRoot 'dist\plugins\LibertyCraft.asi') 'plugins\LibertyCraft.asi'
    foreach ($name in @('LibertyCraft.ini', 'LibertyCraft.pdb')) {
        $source = Join-Path $script:InstallRoot "dist\plugins\$name"
        if (Test-Path -LiteralPath $source) { Install-File $source "plugins\$name" -KeepExisting:($name -eq 'LibertyCraft.ini') }
    }
    foreach ($relative in @('d3d9.cfg', 'plugins\GTAIV.EFLC.FusionFix.cfg', 'LibertyCraft.log')) {
        if (-not $script:Manifest.ContainsKey($relative) -and -not (Test-Path -LiteralPath (Join-Path $script:Game $relative))) { Set-ManifestEntry 'added' $relative }
    }
    Write-Manifest

    if (-not $NoMinecraft) {
        Write-Step 'Configuring Minecraft and Fabric'
        Setup-Prism (Find-PrismDirectory)
    }
    $installedVersion = Get-ExecutableVersion (Join-Path $script:Game 'GTAIV.exe')
    if ($installedVersion -ne '1.0.8.0' -or -not (Test-Path -LiteralPath (Join-Path $script:Game 'xlive.dll'))) {
        Stop-Install 'Final GTA IV verification failed. Review the install log and backup manifest.'
    }
    Write-Step 'Setup complete'
    Write-Host "GTA IV plugin installed under $script:Game\plugins" -ForegroundColor Green
    Write-Host 'Start Prism Launcher once to let it download Minecraft 26.3, Fabric, and Java 25. Sign in first.'
}

if ($env:LIBERTYCRAFT_TEST_MODE -ne '1') {
    try {
        if ($Uninstall) {
            $script:Game = Find-GameDirectory $GameDir
            Read-Manifest
            if ($script:Manifest.Count -eq 0) { Write-Info 'No LibertyCraft game files are recorded for this installation.' }
            else { Restore-GameFiles }
        } else { Invoke-Install }
    } catch {
        Write-Host "`nSetup error: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    } finally {
        if (Test-Path -LiteralPath $script:Stage) { Remove-Item -LiteralPath $script:Stage -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
