$ErrorActionPreference = 'Stop'
$env:LIBERTYCRAFT_TEST_MODE = '1'
. (Join-Path $PSScriptRoot '..\install-windows.ps1')

function Assert-Equal([string]$Expected, [string]$Actual, [string]$Message) {
    if ($Expected -ne $Actual) { throw "$Message. Expected '$Expected', got '$Actual'." }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('libertycraft-installer-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $steam = Join-Path $testRoot 'Steam'
    $library = Join-Path $testRoot 'Games Library'
    $game = Join-Path $library 'steamapps\common\Grand Theft Auto IV\GTAIV'
    New-Item -ItemType Directory -Force -Path (Join-Path $steam 'steamapps'), (Join-Path $library 'steamapps\common\Grand Theft Auto IV\GTAIV') | Out-Null
    Set-Content -LiteralPath (Join-Path $steam 'steamapps\libraryfolders.vdf') -Value @(
        '"libraryfolders" {'
        ' "1" {'
        '  "path" "' + $library.Replace('\', '\\') + '"'
        ' }'
        '}'
    )
    Set-Content -LiteralPath (Join-Path $library 'steamapps\appmanifest_12210.acf') -Value '"AppState" { "installdir" "Grand Theft Auto IV" }'
    Set-Content -LiteralPath (Join-Path $game 'GTAIV.exe') -Value 'fake game executable'

    $script:SteamRootForTest = $steam
    function Get-SteamRoots { return @($script:SteamRootForTest) }
    $detected = Find-GameDirectory
    Assert-Equal $game $detected 'Steam library detection failed'
    Assert-Equal $game (Find-GameDirectory $game) 'Explicit GTA IV path was not accepted'
    $oldAppData = $env:APPDATA
    try {
        $env:APPDATA = $testRoot
        $prism = Join-Path $testRoot 'PrismLauncher'
        New-Item -ItemType Directory -Force -Path $prism | Out-Null
        $script:PrismDir = ''
        Assert-Equal $prism (Find-PrismDirectory) 'Prism data folder detection failed'
    } finally { $env:APPDATA = $oldAppData }

    $script:Game = Join-Path $testRoot 'Install Target'
    New-Item -ItemType Directory -Force -Path $script:Game | Out-Null
    $original = Join-Path $script:Game 'GTAIV.exe'
    Set-Content -LiteralPath $original -Value 'original game file'
    $newFile = Join-Path $testRoot 'GTAIV-new.exe'
    Set-Content -LiteralPath $newFile -Value 'LibertyCraft target file'
    Install-File $newFile 'GTAIV.exe'
    Set-Content -LiteralPath $newFile -Value 'updated LibertyCraft target file'
    Install-File $newFile 'GTAIV.exe'
    $backup = Join-Path (Join-Path $script:Game $script:BackupName) 'GTAIV.exe'
    Assert-Equal 'original game file' (Get-Content -Raw -LiteralPath $backup).Trim() 'Repeat install overwrote the original backup'

    $addedFile = Join-Path $testRoot 'xlive.dll'
    Set-Content -LiteralPath $addedFile -Value 'loader'
    Install-File $addedFile 'plugins\xlive-test.dll'
    Read-Manifest
    Restore-GameFiles
    Assert-Equal 'original game file' (Get-Content -Raw -LiteralPath $original).Trim() 'Uninstall did not restore a replaced file'
    if (Test-Path -LiteralPath (Join-Path $script:Game 'plugins\xlive-test.dll')) { throw 'Uninstall did not delete an added file.' }
    if (Test-Path -LiteralPath (Join-Path $script:Game 'plugins')) { throw 'Uninstall did not remove an installer-created empty directory.' }
    Write-Host 'Windows installer checks passed.' -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
