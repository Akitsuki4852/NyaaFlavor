param([switch]$SkipBuild)
# 构建 GUI + CLI，复制到 artifacts\release，并打一个含 EXE + 许可证声明的 zip。
$ErrorActionPreference = 'Stop'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') gui
    & (Join-Path $PSScriptRoot 'build.ps1') cli
}
$releaseDirectory = Join-Path $PSScriptRoot 'artifacts\release'
New-Item -ItemType Directory -Force -Path $releaseDirectory | Out-Null
Get-ChildItem -LiteralPath $releaseDirectory -File | Remove-Item -Force
$executable = Join-Path $releaseDirectory 'NyaaFlavor.exe'
$cli = Join-Path $releaseDirectory 'NyaaFlavor-cli.exe'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\NyaaFlavor.exe') -Destination $executable -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\NyaaFlavor-cli.exe') -Destination $cli -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\LICENSE') -Destination (Join-Path $releaseDirectory 'LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\THIRD_PARTY_NOTICES.md') -Destination (Join-Path $releaseDirectory 'THIRD_PARTY_NOTICES.md') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\README.md') -Destination (Join-Path $releaseDirectory 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\STYLE.md') -Destination (Join-Path $releaseDirectory 'STYLE.md') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\tinyxml2\LICENSE.txt') -Destination (Join-Path $releaseDirectory 'LICENSE-tinyxml2.txt') -Force
$archive = Join-Path $PSScriptRoot 'artifacts\NyaaFlavor-Cpp.zip'
$files = @($executable, $cli, (Join-Path $releaseDirectory 'README.md'), (Join-Path $releaseDirectory 'LICENSE'),
           (Join-Path $releaseDirectory 'LICENSE-tinyxml2.txt'), (Join-Path $releaseDirectory 'THIRD_PARTY_NOTICES.md'),
           (Join-Path $releaseDirectory 'STYLE.md'))
Compress-Archive -LiteralPath $files -DestinationPath $archive -Force
Write-Host "EXE: $executable  ($((Get-Item -LiteralPath $executable).Length) bytes)"
Write-Host "ZIP: $archive  ($((Get-Item -LiteralPath $archive).Length) bytes)"
