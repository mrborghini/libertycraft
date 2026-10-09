param(
    [string]$PrismDir,
    [string]$PrismExe,
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$script:InstanceId = 'LibertyCraft'
$script:SteamAppId = 12210
$script:ReadyMarker = '[LibertyCraft] game window hidden'

function Stop-Launch([string]$Message) { throw $Message }

function Find-PrismData([string]$Path = '') {
    if ($Path) {
        if (Test-Path -LiteralPath $Path -PathType Container) { return (Resolve-Path -LiteralPath $Path).Path }
        Stop-Launch "The selected Prism data folder does not exist: $Path"
    }
    foreach ($candidate in @(
        (Join-Path $env:APPDATA 'PrismLauncher'),
        (Join-Path $env:LOCALAPPDATA 'PrismLauncher'),
        (Join-Path $env:APPDATA 'Prism Launcher')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Container)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    Stop-Launch 'Prism data folder was not found. Run install-windows.bat first or pass -PrismDir.'
}

function Get-PrismInstanceDirectory([string]$DataDirectory) {
    $instances = Join-Path $DataDirectory 'instances'
    $config = Join-Path $DataDirectory 'prismlauncher.cfg'
    if (Test-Path -LiteralPath $config) {
        $line = Select-String -LiteralPath $config -Pattern '^InstanceDir=(.*)$' | Select-Object -First 1
        if ($line) {
            $configured = $line.Matches[0].Groups[1].Value
            if ([IO.Path]::IsPathRooted($configured)) { $instances = $configured }
            else { $instances = Join-Path $DataDirectory $configured }
        }
    }
    return Join-Path $instances $script:InstanceId
}

function Find-PrismExecutable([string]$DataDirectory, [string]$Path = '') {
    if ($Path) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Resolve-Path -LiteralPath $Path).Path }
        Stop-Launch "The selected Prism executable does not exist: $Path"
    }
    $candidates = [System.Collections.Generic.List[string]]::new()
    $command = Get-Command 'prismlauncher.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $candidates.Add($command.Source) }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if (-not $root) { continue }
        foreach ($folder in @('PrismLauncher', 'Prism Launcher')) {
            $candidates.Add((Join-Path $root "$folder\prismlauncher.exe"))
        }
    }
    foreach ($name in @('prismlauncher.exe', 'PrismLauncher.exe')) {
        $candidates.Add((Join-Path $DataDirectory $name))
    }
    try {
        foreach ($process in Get-CimInstance -ClassName Win32_Process -ErrorAction Stop | Where-Object { $_.Name -ieq 'prismlauncher.exe' -and $_.ExecutablePath }) {
            $candidates.Add($process.ExecutablePath)
        }
    } catch { }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    Stop-Launch 'Prism Launcher executable was not found. Pass its path with -PrismExe.'
}

function Get-LibertyCraftMinecraftProcess {
    try {
        $process = Get-CimInstance -ClassName Win32_Process -ErrorAction Stop |
            Where-Object { $_.Name -in @('java.exe', 'javaw.exe') -and $_.CommandLine -like '*libertycraft.startHidden=true*' } |
            Select-Object -First 1
        return $process
    } catch { return $null }
}

function Get-NewLogText([string]$Previous, [string]$Current) {
    if ($Previous -and $Current.StartsWith($Previous, [StringComparison]::Ordinal)) {
        return $Current.Substring($Previous.Length)
    }
    return $Current
}

function Test-LibertyCraftReady([string]$Text) {
    return $Text.Contains($script:ReadyMarker)
}

function Invoke-LibertyCraftLaunch {
    if ($TimeoutSeconds -lt 1) { Stop-Launch '-TimeoutSeconds must be greater than zero.' }
    $data = Find-PrismData $PrismDir
    $instance = Get-PrismInstanceDirectory $data
    $minecraft = Join-Path $instance 'minecraft'
    $mods = Join-Path $minecraft 'mods'
    $log = Join-Path $minecraft 'logs\latest.log'
    if (-not (Test-Path -LiteralPath $instance -PathType Container)) {
        Stop-Launch "LibertyCraft Prism instance was not found at $instance. Run install-windows.bat first."
    }
    if (-not (Get-ChildItem -LiteralPath $mods -Filter 'libertycraft-*.jar' -File -ErrorAction SilentlyContinue)) {
        Stop-Launch "LibertyCraft mod jar was not found in $mods. Run install-windows.bat again."
    }

    $alreadyRunning = Get-LibertyCraftMinecraftProcess
    $previousLog = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    if ($alreadyRunning) {
        Write-Host 'LibertyCraft Minecraft is already running.'
        if (Test-LibertyCraftReady $previousLog) { $ready = $true }
        else { $ready = $false }
    } else {
        $prism = Find-PrismExecutable $data $PrismExe
        Write-Host "Starting Prism instance '$script:InstanceId'."
        Start-Process -FilePath $prism -ArgumentList @('--launch', $script:InstanceId) | Out-Null
        $ready = $false
    }

    if (-not $ready) {
        Write-Host "Waiting up to $TimeoutSeconds seconds for Minecraft to finish starting."
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 1
            if (Test-Path -LiteralPath $log) {
                $currentLog = Get-Content -LiteralPath $log -Raw
                $newText = Get-NewLogText $previousLog $currentLog
                if (Test-LibertyCraftReady $newText) { $ready = $true; break }
                $previousLog = $currentLog
            }
        }
    }
    if (-not $ready) {
        Stop-Launch "Minecraft did not report that its hidden window started within $TimeoutSeconds seconds. Check the Prism instance log at $log. GTA IV was not started."
    }

    Write-Host 'Minecraft is ready. Starting GTA IV through Steam.'
    Start-Sleep -Seconds 2
    $gta = Get-Process -Name 'GTAIV', 'PlayGTAIV' -ErrorAction SilentlyContinue
    if ($gta) { Write-Host 'GTA IV is already running.'; return }
    Start-Process -FilePath "steam://rungameid/$script:SteamAppId"
}

if ($env:LIBERTYCRAFT_LAUNCH_TEST_MODE -ne '1') {
    try { Invoke-LibertyCraftLaunch }
    catch {
        Write-Host "`nLibertyCraft launch error: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}
