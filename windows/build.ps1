# build.ps1 — build the DS Input Windows frontend end to end.
#
# Steps:
#   1. Build the Rust core (dsime) for the MSVC x64 target -> dsime.dll +
#      import lib.
#   2. Configure + build the C++ TSF DLL and settings exe with CMake (VS 2022).
#   3. Print the registration commands.
#
# Run from a "x64 Native Tools Command Prompt for VS 2022" (PowerShell) so MSVC
# + the Windows SDK are on PATH, or from any PowerShell if those are already set
# up. Requires the Rust msvc toolchain:
#     rustup target add x86_64-pc-windows-msvc
#
# Usage:
#     ./build.ps1                 # release build
#     ./build.ps1 -Config Debug   # debug build

[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir "..")
$CoreDir   = Join-Path $RepoRoot "core"
$Target    = "x86_64-pc-windows-msvc"

Write-Host "==> [1/3] Building Rust core ($Target, $Config)" -ForegroundColor Cyan
Push-Location $CoreDir
try {
    if ($Config -eq "Release") {
        cargo build --release --target $Target
        $CoreOut = Join-Path $CoreDir "target/$Target/release"
    } else {
        cargo build --target $Target
        $CoreOut = Join-Path $CoreDir "target/$Target/debug"
    }
} finally {
    Pop-Location
}
if (-not (Test-Path (Join-Path $CoreOut "dsime.dll"))) {
    throw "core build did not produce dsime.dll in $CoreOut"
}
Write-Host "    core artifacts in $CoreOut" -ForegroundColor DarkGray

Write-Host "==> [2/3] Configuring + building C++ (CMake, VS 2022, x64)" -ForegroundColor Cyan
$BuildDir = Join-Path $ScriptDir "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

cmake -S $ScriptDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
      "-DDSIME_CORE_DIR=$CoreOut"
cmake --build $BuildDir --config $Config

$OutDir = Join-Path $BuildDir $Config
Write-Host "    binaries in $OutDir" -ForegroundColor DarkGray

Write-Host "==> [3/3] Registration" -ForegroundColor Cyan
$Dll = Join-Path $OutDir "dsime_tsf.dll"
Write-Host @"
Build complete.

To register the text service (run from an ELEVATED prompt):
    regsvr32 "$Dll"

To unregister:
    regsvr32 /u "$Dll"

Then enable it in:
    Settings > Time & language > Language & region >
      Chinese (Simplified) > ... > Language options >
      Add a keyboard > "DS Input (LLM Pinyin)"

Set your API key via the language-bar "Settings..." menu, which launches:
    $(Join-Path $OutDir 'DSInputSettings.exe')
"@ -ForegroundColor Green
