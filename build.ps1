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
# cheap and always-correct rule
$headers = @(Get-ChildItem -Path (Join-Path $root 'core'), (Join-Path $root 'gfx') `
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
    param([string]$src)
    $obj = Get-ObjPath $src
    $need = $true
    if (Test-Path -LiteralPath $obj) {
        $objTime = (Get-Item -LiteralPath $obj).LastWriteTime
        if ($objTime -gt (Get-Item -LiteralPath $src).LastWriteTime -and $objTime -gt $newestHeader) {
            $need = $false
        }
    }
    if (-not $need) {
        $script:skipCount++
        return $obj
    }
    Write-Host ("  CXX  " + $src.Substring($root.Length).TrimStart('\', '/'))
    $code = Invoke-Tool -Exe $gxx -ToolArgs (@('-c', $src, '-o', $obj) + $commonFlags)
    if ($code -ne 0) { throw "compile failed: $src" }
    $script:compileCount++
    return $obj
}

# ---------------------------------------------------------------------------
# source sets
# ---------------------------------------------------------------------------
$libSources = @()
$libSources += @(Get-ChildItem -Path (Join-Path $root 'core') -Filter '*.cpp' | ForEach-Object { $_.FullName })
$libSources += @(Get-ChildItem -Path (Join-Path $root 'gfx')  -Filter '*.cpp' | ForEach-Object { $_.FullName })
if ($libSources.Count -eq 0) { Write-Error "no library sources found"; exit 1 }

$buildApp = -not $Cli
$buildCli = -not $App
if (-not $App -and -not $Cli) { $buildApp = $true; $buildCli = $true }

$t0 = Get-Date

Write-Host "[1/3] compiling core + gfx ($($libSources.Count) files)"
$libObjs = @()
foreach ($s in $libSources) { $libObjs += (Invoke-Compile $s) }

if ($buildApp) {
    Write-Host "[2/3] building app"
    $appObj = Invoke-Compile (Join-Path $root 'app\main.cpp')
    $exe = Join-Path $buildDir 'paintboard.exe'
    $code = Invoke-Tool -Exe $gxx -ToolArgs (@($libObjs + $appObj, '-o', $exe) + $linkFlags)
    if ($code -ne 0) { throw "link failed: paintboard.exe" }
    Write-Host ("  LINK " + $exe)
} else {
    Write-Host "[2/3] app skipped"
}

if ($buildCli) {
    Write-Host "[3/3] building offline renderer"
    $cliObj = Invoke-Compile (Join-Path $root 'tools\render_cli.cpp')
    $exe = Join-Path $buildDir 'paintboard_render.exe'
    $code = Invoke-Tool -Exe $gxx -ToolArgs (@($libObjs + $cliObj, '-o', $exe) + $linkFlags)
    if ($code -ne 0) { throw "link failed: paintboard_render.exe" }
    Write-Host ("  LINK " + $exe)
} else {
    Write-Host "[3/3] offline renderer skipped"
}

$dt = (Get-Date) - $t0
Write-Host ("[done] compiled $script:compileCount, cached $script:skipCount, {0:N1}s" -f $dt.TotalSeconds)

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
