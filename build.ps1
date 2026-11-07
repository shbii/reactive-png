# Builds reactive-png.dll with a portable compiler. Nothing is installed system-wide:
# the compiler and OBS headers are downloaded once into .deps\ next to this script.
#
#   .\build.ps1             build into dist\
#   .\build.ps1 -Install    build, then copy into OBS's plugin folder (close OBS first)
#
# The import library is generated from your installed obs.dll, so the plugin always
# links against the OBS you actually run. Set -ObsDir if OBS is not in the default place.
param(
    [string]$ObsDir = "$env:ProgramFiles\obs-studio",
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Name = 'reactive-png'
$LlvmVersion = '20261104'
$LlvmSha256 = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'

$Root = $PSScriptRoot
$Deps = Join-Path $Root '.deps'
$Dist = Join-Path $Root 'dist'
$Tar = "$env:SystemRoot\System32\tar.exe"
New-Item -ItemType Directory -Force $Deps | Out-Null

function Invoke-Native {
    param([string]$Exe, [string[]]$ArgList)
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

$ObsDll = Join-Path $ObsDir 'bin\64bit\obs.dll'
if (-not (Test-Path $ObsDll)) { throw "obs.dll not found at $ObsDll. Pass -ObsDir." }
$ObsVersion = ((Get-Item (Join-Path $ObsDir 'bin\64bit\obs64.exe')).VersionInfo.ProductVersion -split '[-+ ]')[0]

# --- compiler ---------------------------------------------------------------
$Llvm = Join-Path $Deps "llvm-mingw-$LlvmVersion-ucrt-x86_64"
if (-not (Test-Path "$Llvm\bin\clang.exe")) {
    $zip = Join-Path $Deps 'llvm-mingw.zip'
    Write-Host "Downloading llvm-mingw $LlvmVersion..."
    Invoke-WebRequest "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmVersion/llvm-mingw-$LlvmVersion-ucrt-x86_64.zip" -OutFile $zip
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $LlvmSha256) { throw 'llvm-mingw checksum mismatch' }
    Invoke-Native $Tar @('-xf', $zip, '-C', $Deps)
    Remove-Item $zip
}

# --- OBS headers, matching the installed version -----------------------------
$ObsSrc = Join-Path $Deps "obs-studio-$ObsVersion"
if (-not (Test-Path "$ObsSrc\libobs\obs-module.h")) {
    $tgz = Join-Path $Deps 'obs-src.tar.gz'
    Write-Host "Downloading OBS $ObsVersion headers..."
    Invoke-WebRequest "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.tar.gz" -OutFile $tgz
    Invoke-Native $Tar @('-xzf', $tgz, '-C', $Deps, "obs-studio-$ObsVersion/libobs")
    Remove-Item $tgz
}

# --- import library from the installed obs.dll -------------------------------
$Build = Join-Path $Deps "build-$ObsVersion"
New-Item -ItemType Directory -Force $Build | Out-Null
$ImpLib = Join-Path $Build 'libobs.dll.a'
if (-not (Test-Path $ImpLib)) {
    Push-Location $Build
    try {
        Invoke-Native "$Llvm\bin\gendef.exe" @($ObsDll)
        Invoke-Native "$Llvm\bin\llvm-dlltool.exe" @('-m', 'i386:x86-64', '-d', 'obs.def', '-D', 'obs.dll', '-l', $ImpLib)
    } finally { Pop-Location }
}

# --- compile -----------------------------------------------------------------
$OutBin = Join-Path $Dist "$Name\bin\64bit"
New-Item -ItemType Directory -Force $OutBin | Out-Null
# obs-config.h includes this CMake-generated file. Nothing the plugin uses reads it.
Set-Content -Path (Join-Path $Build 'obsconfig.h') -Value '#pragma once' -Encoding ascii
Invoke-Native "$Llvm\bin\clang.exe" @(
    '-std=gnu11', '-O2', '-shared', '-Wall', '-Wextra', '-Wno-unused-parameter',
    # Clang only predefines _M_X64 in MSVC mode. Without it, util/sse-intrin.h
    # falls back to SIMDe, which is not part of libobs.
    '-D_M_X64=100',
    "-I$ObsSrc\libobs", "-I$Build",
    "$Root\src\reactive-png.c", $ImpLib,
    # -static keeps the compiler's runtime in the DLL, so it needs no extra DLLs
    # beside it. A missing one would make OBS skip the plugin with no visible error.
    '-static', '-Wl,--export-all-symbols', '-s',
    '-o', "$OutBin\$Name.dll"
)
Copy-Item -Recurse -Force "$Root\data" (Join-Path $Dist $Name)
Write-Host "Built $OutBin\$Name.dll (linked against OBS $ObsVersion)"

# --- install -----------------------------------------------------------------
if ($Install) {
    if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing.' }
    $Target = Join-Path $env:ProgramData 'obs-studio\plugins'
    New-Item -ItemType Directory -Force $Target | Out-Null
    Copy-Item -Recurse -Force (Join-Path $Dist $Name) $Target
    Write-Host "Installed to $Target\$Name"
}
