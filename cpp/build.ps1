# 用 MSVC 直接编译（不依赖构建系统）。产物为静态链接 CRT 的单文件 exe。
#
#   .\build.ps1 cli      # 命令行版 build\NyaaFlavor-cli.exe
#   .\build.ps1 gui      # GUI 主程序 build\NyaaFlavor.exe
#   .\build.ps1 all      # 两个都编

param([ValidateSet('all', 'cli', 'gui')][string]$Target = "all",
      [ValidateSet('size', 'speed')][string]$Optimize = 'size')

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null

# 用 vswhere 找任意版本的 VS（不再写死 "18\Community"）
$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsPath) {
        $candidate = Join-Path $vsPath.Trim() "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) {
    foreach ($guess in @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")) {
        if (Test-Path $guess) { $vcvars = $guess; break }
    }
}
if (-not $vcvars) { throw "找不到 vcvars64.bat：装 VS 的「使用 C++ 的桌面开发」组件" }

# 公共目标文件（格式层 + 工具层）
$commonSources = @("common.cpp", "nbs.cpp", "pinyin_table.cpp", "tables.cpp", "smf.cpp", "midi2nbs.cpp",
                   "gpvoice.cpp", "postprocess.cpp", "gp2midi.cpp", "cli.cpp", "pipeline.cpp") |
                 ForEach-Object { Join-Path $src $_ }
foreach ($source in $commonSources) {
    if (-not (Test-Path -LiteralPath $source)) { throw "缺少源文件：$source" }
}
$commonSources += Join-Path $root 'third_party\tinyxml2\tinyxml2.cpp'

# 版本资源（右键 exe -> 属性 -> 详细信息 里能看到 0.1.1）
$versionRes = Join-Path $out 'version.res'
$versionRc = Join-Path $src 'version.rc'
$cmd = "`"$vcvars`" >nul 2>&1 && rc /nologo /fo `"$versionRes`" `"$versionRc`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "版本资源编译失败 (exit $LASTEXITCODE)" }
$commonSources += $versionRes

$optimizationFlags = if ($Optimize -eq 'size') { '/O1 /Os' } else { '/O2' }
$cflags = "/nologo /std:c++20 /EHsc /GR- /W4 /permissive- /utf-8 $optimizationFlags /GL /Gy /Gw /GF /Zc:inline /GS /DNDEBUG /DUNICODE /D_UNICODE /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING /MT"
$lflags = "/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /NOLOGO"
$winlibs = "user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib comdlg32.lib advapi32.lib uuid.lib"

function Invoke-Cl($argsList) {
    # 在 build/ 里跑 cl，避免把 /Fo、/Fd 拼成带空格的路径；源码逐个加引号。
    $cmd = "`"$vcvars`" >nul 2>&1 && cd /d `"$out`" && cl $argsList"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "编译失败 (exit $LASTEXITCODE)" }
}

if ($Target -in @("cli", "all")) {
    Write-Host "== 构建命令行版" -ForegroundColor Cyan
    $sourceList = @($commonSources) + (Join-Path $src "cli_main.cpp")
    $sources = ($sourceList | ForEach-Object { '"' + $_ + '"' }) -join ' '
    Invoke-Cl "$cflags $sources /link $lflags /SUBSYSTEM:CONSOLE /OUT:`"$out\NyaaFlavor-cli.exe`""
    $f = Get-Item "$out\NyaaFlavor-cli.exe"
    Write-Host ("   NyaaFlavor-cli.exe  {0:N0} 字节 ({1:N2} MB)" -f $f.Length, ($f.Length / 1MB)) -ForegroundColor Green
}

if ($Target -in @("gui", "all")) {
    Write-Host "== 构建 GUI" -ForegroundColor Cyan
    $sourceList = @($commonSources) + (Join-Path $src "gui.cpp")
    $sources = ($sourceList | ForEach-Object { '"' + $_ + '"' }) -join ' '
    Invoke-Cl "$cflags $sources /link $lflags /SUBSYSTEM:WINDOWS /OUT:`"$out\NyaaFlavor.exe`" /MANIFEST:EMBED /MANIFESTINPUT:`"$root\app.manifest`" $winlibs"
    $f = Get-Item "$out\NyaaFlavor.exe"
    Write-Host ("   NyaaFlavor.exe  {0:N0} 字节 ({1:N2} MB)" -f $f.Length, ($f.Length / 1MB)) -ForegroundColor Green
}
