# build.ps1 — build the DS Input Windows frontend for one or more architectures.
#
# For each requested arch it:
#   1. Builds the Rust core (dsime) for the MSVC target -> dsime.dll + import lib.
#   2. Configures + builds the C++ TSF DLL and settings exe with CMake (VS 2022).
#   3. Stages the trio (dsime_tsf.dll, dsime.dll, DSInputSettings.exe) under
#      windows/dist/<arch>/ — the layout the installer bundles from.
#
# Run from a "x64 Native Tools Command Prompt for VS 2022" (PowerShell) or any
# PowerShell where MSVC + the Windows SDK + the Rust msvc toolchain are set up.
#
# Usage:
#   ./build.ps1                       # build every supported arch (x64 + arm64)
#   ./build.ps1 -Arch x64             # just one
#   ./build.ps1 -Arch arm64 -Config Debug
#
# A requested arch whose Rust target or MSVC compiler is missing is skipped with
# a warning (so an x64-only CI runner still produces the x64 build).

[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    # "all" (default) builds x64 + arm64; or pick one.
    [ValidateSet("all", "x64", "arm64")]
    [string]$Arch = "all"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir "..")
$CoreDir   = Join-Path $RepoRoot "core"
$DistDir   = Join-Path $ScriptDir "dist"

# arch name -> (rust target triple, CMake -A value)
$Arches = @{
    "x64"   = @{ Target = "x86_64-pc-windows-msvc";  CmakeA = "x64"   }
    "arm64" = @{ Target = "aarch64-pc-windows-msvc"; CmakeA = "ARM64" }
}
$Selected = if ($Arch -eq "all") { @("x64", "arm64") } else { @($Arch) }

function Test-RustTarget([string]$triple) {
    $installed = (rustup target list --installed) 2>$null
    if ($installed -notcontains $triple) {
        Write-Host "    installing rust target $triple…" -ForegroundColor DarkGray
        rustup target add $triple | Out-Null
    }
    return ((rustup target list --installed) -contains $triple)
}

$built = @()
foreach ($a in $Selected) {
    $triple = $Arches[$a].Target
    $cmakeA = $Arches[$a].CmakeA
    Write-Host "==> [$a] core ($triple, $Config)" -ForegroundColor Cyan

    if (-not (Test-RustTarget $triple)) {
        Write-Warning "skipping $a — rust target $triple unavailable."
        continue
    }

    Push-Location $CoreDir
    try {
        if ($Config -eq "Release") {
            cargo build --release --target $triple
            $CoreOut = Join-Path $CoreDir "target/$triple/release"
        } else {
            cargo build --target $triple
            $CoreOut = Join-Path $CoreDir "target/$triple/debug"
        }
    } catch {
        Pop-Location
        Write-Warning "skipping $a — core build failed: $_"
        continue
    }
    Pop-Location
    if (-not (Test-Path (Join-Path $CoreOut "dsime.dll"))) {
        Write-Warning "skipping $a — core did not produce dsime.dll."
        continue
    }

    Write-Host "==> [$a] C++ (CMake, VS 2022, $cmakeA)" -ForegroundColor Cyan
    $BuildDir = Join-Path $ScriptDir "build/$a"
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    try {
        cmake -S $ScriptDir -B $BuildDir -G "Visual Studio 17 2022" -A $cmakeA `
              "-DDSIME_CORE_DIR=$CoreOut"
        cmake --build $BuildDir --config $Config
    } catch {
        Write-Warning "skipping $a — CMake build failed (is the $cmakeA MSVC toolset installed?): $_"
        continue
    }

    $OutDir = Join-Path $BuildDir $Config
    $Stage  = Join-Path $DistDir $a
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    foreach ($f in @("dsime_tsf.dll", "DSInputSettings.exe")) {
        Copy-Item (Join-Path $OutDir $f) (Join-Path $Stage $f) -Force
    }
    Copy-Item (Join-Path $CoreOut "dsime.dll") (Join-Path $Stage "dsime.dll") -Force
    Write-Host "    staged -> $Stage" -ForegroundColor DarkGray
    $built += $a
}

if ($built.Count -eq 0) {
    throw "No architectures built. Check the Rust + MSVC toolchains."
}

Write-Host ""
Write-Host "Built arch(es): $($built -join ', ')" -ForegroundColor Green
Write-Host "Staged under: $DistDir\<arch>\ (dsime_tsf.dll, dsime.dll, DSInputSettings.exe)"
Write-Host ""
Write-Host "To register a build manually (ELEVATED prompt, matching-arch regsvr32):" -ForegroundColor Green
foreach ($a in $built) {
    Write-Host "    regsvr32 `"$DistDir\$a\dsime_tsf.dll`""
}
Write-Host ""
Write-Host "Or build + run the guided installer (installs the host-matching arch):"
Write-Host "    ./installer/build-installer.ps1 ; ./installer/build/DSInputInstaller.exe"
