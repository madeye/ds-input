# DS Input — Windows installer

A single, self-contained, elevated **universal installer** (`DSInputInstaller.exe`)
for the DS Input TSF IME — the Windows counterpart to the macOS guided installer.

It **embeds both the x64 and ARM64 builds**, detects the host architecture at run
time, installs the matching set, registers the text service, and adds DS Input to
the user's Chinese (Simplified) keyboards.

> Windows has no fat/universal binaries: a TSF text service is an in-proc COM DLL
> loaded into each text-host process and must match that process's architecture.
> This installer ships per-arch DLLs and installs the one matching the host OS, so
> one download works on both x64 and ARM64 PCs. (Serving x64-emulated apps on
> ARM64, or 32-bit apps on x64, would need the extra-arch DLL / an ARM64X hybrid —
> a future enhancement.)

## What it does

1. Extracts the host-arch payload (`dsime_tsf.dll`, `dsime.dll`,
   `DSInputSettings.exe`) to `%ProgramFiles%\DSInput`.
2. `regsvr32` the TSF DLL (the native-arch `regsvr32` matches the native-arch DLL).
3. Adds the DS Input profile to the user's `zh-Hans` language list.
4. Tells the user to sign out / back in — Windows enrolls a freshly registered
   text service at the next logon.

## Build

From a VS 2022 environment with both MSVC toolsets + the Rust MSVC targets:

```powershell
cd windows
./installer/build-installer.ps1            # runs build.ps1 -Arch all, then packages
# -> installer/build/Release/DSInputInstaller.exe
```

`build-installer.ps1 -SkipBuild` repackages whatever is already staged in
`windows/dist/<arch>/` (handy when iterating on the installer itself).

## Run

Double-click `DSInputInstaller.exe` (it prompts for administrator). For automation
or headless verification, run it silently:

```powershell
DSInputInstaller.exe /S        # installs without UI; exit 0 = success, 1 = failure
```

## Files

| File | Role |
|------|------|
| `DSInputInstaller.cpp` | Wizard UI + install logic (extract, register, language list); `/S` silent mode. |
| `DSInputInstaller.rc` | Dialog template, icon, embedded manifest, and the embedded per-arch payloads. |
| `installer.manifest` | `requireAdministrator` + common-controls + DPI awareness. |
| `CMakeLists.txt` | Builds the installer exe (x64) with `/MANIFEST:NO` (manifest comes from the .rc). |
| `build-installer.ps1` | Builds both arches then packages the installer. |
