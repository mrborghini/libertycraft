$ErrorActionPreference = 'Stop'
$env:LIBERTYCRAFT_TEST_MODE = '1'
. (Join-Path $PSScriptRoot '..\install-windows.ps1')

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('libertycraft-downgrade-assets-' + [guid]::NewGuid().ToString('N'))
$script:Cache = $testRoot
$script:Stage = Join-Path $testRoot 'stage'
New-Item -ItemType Directory -Force -Path $testRoot, $script:Stage | Out-Null
try {
    $archive = Join-Path $testRoot 'BaseAssets.zip'
    Get-Download $script:BaseAssetsUrl $archive $script:BaseAssetsSha256
    $extracted = Join-Path $script:Stage 'base'
    Expand-ZipSelection $archive $extracted @() @('1080/GTAIV.exe')
    $exe = Join-Path $extracted '1080\GTAIV.exe'
    Assert-ExecutableVersionAndSize $exe 'Downloaded downgrade executable' $script:DowngradeExecutableLength $script:DowngradeExecutableVersion
    Write-Host "Pinned downgrade executable passed validation: $((Get-Item -LiteralPath $exe).Length) bytes, version $(Get-ExecutableVersion $exe)." -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
