param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$GameRoot
)
$ErrorActionPreference = 'Stop'
$sourcePath = [IO.Path]::GetFullPath($Source)
$root = [IO.Path]::GetFullPath($GameRoot)
$target = Join-Path $root 'hid.dll'
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "Integrated hid.dll build is missing: $sourcePath"
}
# 常规部署只负责当前产物；旧安装的清理由独立的一次性迁移步骤处理。
if (Get-Process -Name MonsterHunterWilds -ErrorAction SilentlyContinue) {
    throw 'Game is running: integrated HID/ClassInjector deployment deferred; no files changed.'
}
if (Test-Path -LiteralPath $target) {
    $stream = [IO.File]::Open($target, 'Open', 'ReadWrite', 'None')
    $stream.Dispose()
}
$sha = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
New-Item -ItemType Directory -Path $root -Force | Out-Null
Copy-Item -LiteralPath $sourcePath -Destination $target -Force
if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $sha) {
    throw "Integrated hid.dll deployment SHA mismatch: $target"
}
Write-Output "Integrated hid.dll deployed: $target SHA256=$sha. Runtime loading still requires cold-start validation."
