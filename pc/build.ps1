# pc/build.ps1
#
# Configures and builds pc_capture_spike using the MSVC toolset + CMake +
# Ninja that ship inside Visual Studio (no separate CMake/Ninja install
# needed if VS Community/Build Tools with the "Desktop development with
# C++" workload is present).
#
# Usage (from anywhere):
#   pwsh -File pc\build.ps1
#   pwsh -File pc\build.ps1 -Run          # also runs the exe after building

param(
    [switch]$Run
)

$ErrorActionPreference = "Stop"

$RepoPc = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $RepoPc "build"

# Locate a Visual Studio install with the C++ toolset via vswhere (installed
# alongside every VS 2022 edition, including Build Tools).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Install Visual Studio 2022 " +
          "(Community is free) with the 'Desktop development with C++' " +
          "workload -- see pc/README.md."
}

$vsInstallPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsInstallPath) {
    throw "No Visual Studio install found with the C++ (VC.Tools.x86.x64) " +
          "component. Install the 'Desktop development with C++' workload " +
          "-- see pc/README.md."
}

$vcvars64 = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
$cmakeExe = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaExe = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

foreach ($p in @($vcvars64, $cmakeExe, $ninjaExe)) {
    if (-not (Test-Path $p)) {
        throw "Expected file missing: '$p'. Your VS install may be missing " +
              "the CMake/Ninja optional components -- see pc/README.md."
    }
}

# vcvars64.bat only sets environment variables in its own process, so we
# have to run the whole configure+build inside one cmd.exe invocation that
# calls it first. Absolute paths are passed to -S/-B so this works no
# matter what directory the cmd.exe subprocess starts in.
$cmd = @"
@echo off
call "$vcvars64" >nul
if errorlevel 1 (echo vcvars64.bat failed & exit /b 1)
"$cmakeExe" -S "$RepoPc" -B "$BuildDir" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninjaExe" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"$cmakeExe" --build "$BuildDir"
"@

$tmpBat = Join-Path $env:TEMP "sidepad_pc_capture_build_$PID.bat"
Set-Content -Path $tmpBat -Value $cmd -Encoding ASCII
try {
    & cmd.exe /c $tmpBat
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed (exit code $LASTEXITCODE). See output above."
    }
} finally {
    Remove-Item $tmpBat -ErrorAction SilentlyContinue
}

Write-Host "`nBuild OK: $BuildDir\pc_capture_spike.exe"

if ($Run) {
    & (Join-Path $BuildDir "pc_capture_spike.exe")
}
