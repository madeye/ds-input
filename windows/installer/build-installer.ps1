# build-installer.ps1 — build the universal DS Input installer.
#
#   1. Run ../build.ps1 to produce both arch builds under ../dist/<arch>/.
#   2. Verify both arches are present (a universal installer needs both).
#   3. Compile the installer exe (x64, so it runs on x64 and ARM64) with both
#      payloads embedded.
#
# Output: installer/build/<Config>/DSInputInstaller.exe — a single self-contained,
# elevated installer.

[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    # Skip the (slow) core+frontend rebuild and just package whatever is already
    # staged in ../dist (useful when iterating on the installer itself).
    [switch]$SkipBuild,
    # CMake generator (see ../build.ps1). Default VS generator works from a dev
    # prompt; CI passes "Ninja" with the x64 MSVC env active (the installer is
    # always built x64 so one exe runs on x64 natively and ARM64 emulated).
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$Dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Win = Resolve-Path (Join-Path $Dir "..")

if (-not $SkipBuild) {
    Write-Host "==> Building both arch payloads (build.ps1 -Arch all)" -ForegroundColor Cyan
    & (Join-Path $Win "build.ps1") -Arch all -Config $Config -Generator $Generator
}

Write-Host "==> Checking staged payloads" -ForegroundColor Cyan
$files = @("dsime_tsf.dll", "dsime.dll", "DSInputSettings.exe")
foreach ($a in @("x64", "arm64")) {
    foreach ($f in $files) {
        $p = Join-Path $Win "dist/$a/$f"
        if (-not (Test-Path $p)) {
            throw "missing $p — a universal installer needs both x64 and arm64. " +
                  "Run build.ps1 -Arch all on a machine with both MSVC toolsets."
        }
    }
}

Write-Host "==> Compiling the installer (x64, $Generator)" -ForegroundColor Cyan
$Build = Join-Path $Dir "build"
New-Item -ItemType Directory -Force -Path $Build | Out-Null
# cmake is a native command — check $LASTEXITCODE (it doesn't throw on failure).
if ($Generator -like "Visual Studio*") {
    cmake -S $Dir -B $Build -G $Generator -A x64
    if ($LASTEXITCODE -ne 0) { throw "installer cmake configure failed (exit $LASTEXITCODE)" }
    cmake --build $Build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "installer cmake build failed (exit $LASTEXITCODE)" }
    $Out = Join-Path $Build "$Config/DSInputInstaller.exe"
} else {
    cmake -S $Dir -B $Build -G $Generator "-DCMAKE_BUILD_TYPE=$Config"
    if ($LASTEXITCODE -ne 0) { throw "installer cmake configure failed (exit $LASTEXITCODE)" }
    cmake --build $Build
    if ($LASTEXITCODE -ne 0) { throw "installer cmake build failed (exit $LASTEXITCODE)" }
    $Out = Join-Path $Build "DSInputInstaller.exe"
}
Write-Host ""
Write-Host "Installer built: $Out" -ForegroundColor Green
Write-Host "Run it (double-click; it will prompt for administrator)."
