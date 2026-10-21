# build.ps1 -- build the 2D pixel-art paintboard
#
# NOTE: this script is intentionally ASCII-only. Windows PowerShell 5.1 parses
# BOM-less files as ANSI, which would corrupt non-ASCII text in the script itself.
#
# Usage:
#   .\build.ps1                 # build everything (app + offline renderer)
#   .\build.ps1 -Cli            # only the offline renderer
#   .\build.ps1 -App            # only the interactive app
#   .\build.ps1 -Clean          # remove build/ and rebuild from scratch
#   .\build.ps1 -Gxx <path>     # use a specific g++.exe
#   .\build.ps1 -Run            # build the app, then launch it

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$App,
    [switch]$Cli,
    [switch]$Run,
    [switch]$Test,
    [string]$Gxx = '',
    [string]$Config = 'release'   # release | debug
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location -LiteralPath $root

$buildDir = Join-Path $root 'build'
$objDir   = Join-Path $buildDir 'obj'

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Host "[clean] removing $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

# ---------------------------------------------------------------------------
# locate the compiler (Red Panda C++ bundles MinGW-w64; that is what this
# project is built with)
# ---------------------------------------------------------------------------
function Find-Gxx {
    param([string]$Hint)
    $cands = New-Object System.Collections.Generic.List[string]
    if ($Hint) { $cands.Add($Hint) }
    if ($env:REDPANDA_HOME) { $cands.Add((Join-Path $env:REDPANDA_HOME 'mingw64\bin\g++.exe')) }
    $cands.Add('D:\Program Files\RedPanda-Cpp\mingw64\bin\g++.exe')
    $cands.Add('C:\Program Files\RedPanda-Cpp\mingw64\bin\g++.exe')
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    $onPath = Get-Command g++ -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

$gxx = Find-Gxx -Hint $Gxx
if (-not $gxx) {
    Write-Error "g++ not found. Pass -Gxx <path\to\g++.exe> or set REDPANDA_HOME."
    exit 1
}
Write-Host "[toolchain] $gxx"

# CRITICAL: the compiler's own bin directory must be on PATH.
# g++.exe is only a driver; it spawns cc1plus.exe from ../libexec/gcc/... and
# cc1plus links against the MinGW math runtime DLLs (libmpc/libmpfr/libgmp/...)
# which live in this same bin directory. Invoking g++.exe by absolute path
# without it on PATH makes cc1plus die at load time with 0xC000007B
# (STATUS_INVALID_IMAGE_FORMAT) and g++ reports only a bare exit code 1 -- no
# diagnostic at all. Prepending the directory here makes the build independent
# of how the caller's PATH happens to be set up.
$gxxDir = Split-Path -Parent $gxx
if (($env:PATH -split ';') -notcontains $gxxDir) {
    $env:PATH = "$gxxDir;$env:PATH"
    Write-Host "[toolchain] prepended to PATH: $gxxDir"
}

Write-Host ("[toolchain] " + ((& $gxx --version | Select-Object -First 1)))

New-Item -ItemType Directory -Force -Path $objDir | Out-Null

# ---------------------------------------------------------------------------
# flags
# ---------------------------------------------------------------------------
# exec-charset=UTF-8 (not the original project's gbk) keeps string literals as
# UTF-8; the binaries call SetConsoleOutputCP(CP_UTF8) at startup so Chinese
# output renders correctly in both conhost and Windows Terminal.
$commonFlags = @(
    '-std=c++17',
    '-fno-ms-extensions',
    '-pipe',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-Werror=return-type',
    '-Werror=vla',
    '-finput-charset=UTF-8',
    '-fexec-charset=UTF-8',
    "-I$root"
)
# Third-party headers go through -isystem: GCC then suppresses its own warnings
# for them (stb's `{ 0 }` initializers trip -Wmissing-field-initializers), so our
# -Wall -Wextra stays useful on our own code.
$commonFlags += @('-isystem', (Join-Path $root 'third_party\stb'))
$commonFlags += @('-isystem', (Join-Path $root 'third_party\imgui'))
$commonFlags += @('-isystem', (Join-Path $root 'third_party\imgui\backends'))
if ($Config -eq 'debug') {
    $commonFlags += @('-O0', '-g', '-DDEBUG')
} else {
    $commonFlags += @('-O2', '-DNDEBUG')
}

$linkFlags = @(
    '-Wl,--stack,12582912',
    '-lglfw3',
    '-lglew32',
    '-lopengl32',
    '-lwinmm',
    '-lgdi32',
    '-static',
    '-static-libgcc',
    '-static-libstdc++'
)
if ($Config -eq 'release') { $linkFlags += '-s' }

# ---------------------------------------------------------------------------
# incremental compile helpers
# ---------------------------------------------------------------------------
# any header change invalidates every object; the tree is small so this is a
# cheap and always-correct rule. app/ and ui/ must be included too: changing
# app/app.h (the shared state struct) otherwise leaves every dependent .o stale.
# tools/ is on the list for the same reason: tools/common.h is shared by
# several tools, so a change there must rebuild the tool objects too.
$headers = @(Get-ChildItem -Path (Join-Path $root 'core'), (Join-Path $root 'gfx'),
                (Join-Path $root 'ui'), (Join-Path $root 'app'), (Join-Path $root 'tools') `
                -Recurse -Include '*.h', '*.hpp' -ErrorAction SilentlyContinue)
$newestHeader = [datetime]::MinValue
foreach ($h in $headers) { if ($h.LastWriteTime -gt $newestHeader) { $newestHeader = $h.LastWriteTime } }

$script:compileCount = 0
$script:skipCount = 0
$script:tmpErrs = Join-Path $buildDir 'build-diagnostics.txt'

# Run a native tool and surface its diagnostics.
#
# PowerShell 5.1 turns anything a native command writes to stderr into a
# *terminating* error while $ErrorActionPreference is 'Stop'. g++ writes all of
# its diagnostics to stderr, so with 'Stop' active the build dies on the first
# warning and the actual message is swallowed. Dropping to 'Continue' for the
# duration of the call keeps the output flowing as ordinary text.
function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = & $Exe @ToolArgs 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($null -ne $lines) {
        foreach ($l in $lines) { Write-Host "        $l" }
    }
    return $code
}

function Get-ObjPath {
    param([string]$src)
    # core/canvas.cpp -> build/obj/core_canvas.o   (keeps core/ and gfx/ apart)
    $rel = $src.Substring($root.Length).TrimStart('\', '/')
    $name = ($rel -replace '[\\/]', '_') -replace '\.cpp$', '.o'
    return (Join-Path $objDir $name)
}

function Invoke-Compile {
    param([string]$src, [string[]]$ExtraFlags = @())
    $obj = Get-ObjPath $src
    $need = $true
    if (Test-Path -LiteralPath $obj) {
        $objTime = (Get-Item -LiteralPath $obj).LastWriteTime
        if ($objTime -gt (Get-Item -LiteralPath $src).LastWriteTime -and $objTime -gt $newestHeader) {
            $need = $false
        }
    }
    if (-not $need) {
        $script:skipCount += 1
        return $obj
    }
    Write-Host ("  CXX  " + $src.Substring($root.Length).TrimStart('\', '/'))
    $code = Invoke-Tool -Exe $gxx -ToolArgs (@('-c', $src, '-o', $obj) + $commonFlags + $ExtraFlags)
    if ($code -ne 0) { throw "compile failed: $src" }
    $script:compileCount += 1
    return $obj
}

# ---------------------------------------------------------------------------
# source sets
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 三个静态库 + 前端
#
#   libpb_render.a   core/ + gfx/      渲染器本体：不依赖 app/ui/ImGui
#   libpb_scenes.a   scenes/           示例内容（bench 场景）
#   libpb_ui.a       ui/ + Dear ImGui  前端控件
#
# 前端（paintboard）与工具（bench_view / render_cli / ...）只链接这三个库，
# 不再共享零散的 .o —— **库边界就是依赖边界**：哪天 gfx 里偷偷 include 了
# app/ 的头，链接期就会缺符号，而不是靠人盯。
# ---------------------------------------------------------------------------
$renderSources = @()
$renderSources += @(Get-ChildItem -Path (Join-Path $root 'core') -Filter '*.cpp' | ForEach-Object { $_.FullName })
$renderSources += @(Get-ChildItem -Path (Join-Path $root 'gfx')  -Filter '*.cpp' | ForEach-Object { $_.FullName })
$sceneSources = @(Get-ChildItem -Path (Join-Path $root 'scenes') -Filter '*.cpp' -ErrorAction SilentlyContinue |
                  ForEach-Object { $_.FullName })
$uiSources = @(Get-ChildItem -Path (Join-Path $root 'ui') -Filter '*.cpp' | ForEach-Object { $_.FullName })
if ($renderSources.Count -eq 0) { Write-Error "no renderer sources found"; exit 1 }

# Dear ImGui is compiled as its own set of translation units with -w: it is
# upstream code we do not modify, and its warnings would drown out ours.
$imguiSources = @(
    'third_party\imgui\imgui.cpp',
    'third_party\imgui\imgui_draw.cpp',
    'third_party\imgui\imgui_tables.cpp',
    'third_party\imgui\imgui_widgets.cpp',
    'third_party\imgui\backends\imgui_impl_glfw.cpp',
    'third_party\imgui\backends\imgui_impl_opengl3.cpp'
) | ForEach-Object { Join-Path $root $_ }
# the official demo window is a large extra TU; only useful while developing UI
if ($Config -eq 'debug') { $imguiSources += (Join-Path $root 'third_party\imgui\imgui_demo.cpp') }
foreach ($s in $imguiSources) {
    if (-not (Test-Path -LiteralPath $s)) { Write-Error "missing vendored source: $s"; exit 1 }
}

$buildApp = -not $Cli
$buildCli = -not $App
if (-not $App -and -not $Cli) { $buildApp = $true; $buildCli = $true }

$t0 = Get-Date
$ar = Join-Path $gxxDir 'ar.exe'
if (-not (Test-Path -LiteralPath $ar)) { $ar = 'ar' }

function New-StaticLib($name, $objs) {
    if ($objs.Count -eq 0) { return $null }
    $lib = Join-Path $buildDir ($name + '.a')
    if (Test-Path -LiteralPath $lib) { Remove-Item -LiteralPath $lib -Force }
    $code = Invoke-Tool -Exe $ar -ToolArgs (@('rcs', $lib) + $objs)
    if ($code -ne 0) { throw "ar failed: $name" }
    Write-Host ("  AR   " + $lib)
    return $lib
}

Write-Host "[1/4] renderer (core + gfx): $($renderSources.Count) files"
$renderObjs = @()
foreach ($s in $renderSources) { $renderObjs += (Invoke-Compile $s) }
$renderLib = New-StaticLib 'libpb_render' $renderObjs

$sceneLib = $null
if ($sceneSources.Count -gt 0) {
    Write-Host "[2/4] scenes: $($sceneSources.Count) files"
    $sceneObjs = @()
    foreach ($s in $sceneSources) { $sceneObjs += (Invoke-Compile $s) }
    $sceneLib = New-StaticLib 'libpb_scenes' $sceneObjs
} else {
    Write-Host "[2/4] scenes: none"
}

Write-Host "[3/4] ui + Dear ImGui: $($uiSources.Count + $imguiSources.Count) files"
$uiObjs = @()
foreach ($s in $uiSources) { $uiObjs += (Invoke-Compile $s) }
foreach ($s in $imguiSources) { $uiObjs += (Invoke-Compile $s @('-w')) }
$uiLib = New-StaticLib 'libpb_ui' $uiObjs

# 链接顺序：前端对象 -> ui -> scenes -> render（被依赖的库放后面）
$libLinkFlags = @()
if ($uiLib) { $libLinkFlags += $uiLib }
if ($sceneLib) { $libLinkFlags += $sceneLib }
$libLinkFlags += $renderLib

if ($buildApp) {
    Write-Host "[4/4] link app"
    $appSources = @(Get-ChildItem -Path (Join-Path $root 'app') -Filter '*.cpp' |
                    Sort-Object Name | ForEach-Object { $_.FullName })
    $appObjs = @()
    foreach ($s in $appSources) { $appObjs += (Invoke-Compile $s) }
    $exe = Join-Path $buildDir 'paintboard.exe'
    # 参数列表必须显式拼：`@($libObjs + $appObjs, '-o', $exe)` 会变成嵌套数组，
    # PowerShell 把它当一个空格分隔的参数传给 ld，报 "cannot find a.o b.o"。
    $linkArgs = @()
    $linkArgs += $appObjs
    $linkArgs += $libLinkFlags
    $linkArgs += @('-o', $exe)
    $linkArgs += $linkFlags
    $code = Invoke-Tool -Exe $gxx -ToolArgs $linkArgs
    if ($code -ne 0) { throw "link failed: paintboard.exe" }
    Write-Host ("  LINK " + $exe)
} else {
    Write-Host "[4/4] app skipped"
}

if ($buildCli) {
    # every tools/*.cpp becomes its own executable (offline renderer, core tests, ...)
    $toolSources = @(Get-ChildItem -Path (Join-Path $root 'tools') -Filter '*.cpp' |
                     Sort-Object Name | ForEach-Object { $_.FullName })
    Write-Host ("      building tools ($($toolSources.Count))")
    foreach ($t in $toolSources) {
        $obj = Invoke-Compile $t
        $name = [System.IO.Path]::GetFileNameWithoutExtension($t)
        $exe = Join-Path $buildDir ($name + '.exe')
        $linkArgs = @()
        $linkArgs += $obj
        $linkArgs += $libLinkFlags
        $linkArgs += @('-o', $exe)
        $linkArgs += $linkFlags
        $code = Invoke-Tool -Exe $gxx -ToolArgs $linkArgs
        if ($code -ne 0) { throw "link failed: $name" }
        Write-Host ("  LINK " + $exe)
    }
} else {
    Write-Host "      tools skipped"
}
# --- 把运行期资源复制到 build\ 旁边 ---
#
# 为什么必须做这一步：UI 字体是在**运行时**从磁盘读的（core/paths.cpp 的
# findAsset 会依次找 当前目录 -> exe 目录 -> exe 的上一级）。在项目里跑能靠
# "上一级就是仓库根"找到字体，但**一旦把 exe 拷到别的机器/别的目录，上一级
# 就没有字体了**，界面中文会全变成 '?'。
#
# 所以资源要在构建时就放到 exe 同目录 —— 这样把 build\ 整个拷走就能直接用。
$assetNames = @('unifont-17.0.04.otf')
foreach ($an in $assetNames) {
    $src = Join-Path $root $an
    $dst = Join-Path $buildDir $an
    if (Test-Path -LiteralPath $src) {
        if ((Test-Path -LiteralPath $dst) -and
            ((Get-Item -LiteralPath $dst).Length -eq (Get-Item -LiteralPath $src).Length)) {
            Write-Host ("  asset " + $an + " (already beside exe)")
        } else {
            Copy-Item -LiteralPath $src -Destination $dst -Force
            Write-Host ("  ASSET " + $an + " -> build\")
        }
    } else {
        Write-Warning ("asset missing, UI CJK text will fall back to '?': " + $src)
    }
}

$dt = (Get-Date) - $t0
Write-Host ("[done] compiled $script:compileCount, cached $script:skipCount, {0:N1}s" -f $dt.TotalSeconds)

if ($Test) {
    $exe = Join-Path $buildDir 'core_test.exe'
    if (Test-Path -LiteralPath $exe) {
        Write-Host "[test] $exe"
        & $exe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } else {
        Write-Error "core_test.exe was not built"
        exit 1
    }
}

if ($Run) {
    $exe = Join-Path $buildDir 'paintboard.exe'
    if (Test-Path -LiteralPath $exe) {
        Write-Host "[run] $exe"
        & $exe
    } else {
        Write-Error "paintboard.exe was not built"
        exit 1
    }
}
