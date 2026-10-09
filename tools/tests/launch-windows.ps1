$ErrorActionPreference = 'Stop'
$env:LIBERTYCRAFT_LAUNCH_TEST_MODE = '1'
. (Join-Path $PSScriptRoot '..\launch-windows.ps1')

function Assert-Equal([string]$Expected, [string]$Actual, [string]$Message) {
    if ($Expected -ne $Actual) { throw "$Message. Expected '$Expected', got '$Actual'." }
}

$marker = '[LibertyCraft] game window hidden'
$oldLog = "old run`r`n$marker"
$appendedLog = "$oldLog`r`nMinecraft started"
$newText = Get-NewLogText $oldLog $appendedLog
if (Test-LibertyCraftReady $newText) { throw 'An old readiness marker was treated as a new Minecraft start.' }
if (-not (Test-LibertyCraftReady "$newText`r`n$marker")) { throw 'A new readiness marker was not detected.' }
$replacedLog = "new run`r`n$marker"
Assert-Equal $replacedLog (Get-NewLogText $oldLog $replacedLog) 'A replaced Minecraft log was not read from its start'
Write-Host 'Windows launch ordering checks passed.' -ForegroundColor Green
