param([Parameter(Mandatory)][string]$GameRoot)
$ErrorActionPreference = 'Stop'
# 一次性迁移：只撤掉被集成进 hid.dll 的旧 host，不参与常规构建或部署。
$target = Join-Path ([IO.Path]::GetFullPath($GameRoot)) 'reframework/plugins/ClassInjector.dll'
if (-not (Test-Path -LiteralPath $target)) {
    Write-Output "Standalone ClassInjector host already absent: $target"
    exit 0
}
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    throw "Expected a standalone host file: $target"
}
if (Get-Process -Name MonsterHunterWilds -ErrorAction SilentlyContinue) {
    throw 'Game is running: standalone host removal deferred; no files changed.'
}
Remove-Item -LiteralPath $target
Write-Output "Removed standalone ClassInjector host: $target"
