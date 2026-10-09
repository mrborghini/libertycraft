$ErrorActionPreference = 'Stop'
$env:LIBERTYCRAFT_TEST_MODE = '1'
. (Join-Path $PSScriptRoot '..\install-windows.ps1')

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('libertycraft-fabric-api-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $api = Join-Path $testRoot $script:FabricApiJarName
    Get-Download $script:FabricApiUrl $api $script:FabricApiSha512 -HashAlgorithm 'SHA512'
    Write-Host "Pinned Fabric API passed SHA-512 verification: $($script:FabricApiJarName)." -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
