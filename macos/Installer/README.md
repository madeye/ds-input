# DS Input Installer

A guided, **per-user** installer for the DS Input IME — modeled on WeType's
installer, but with no admin privileges and no network download.

## What it does

A 3-step wizard (`WizardView.swift`, driven by `InstallerModel.swift`):

1. **Install** — copies the embedded `DSInput.app` into
   `~/Library/Input Methods/` (no password), registers it with LaunchServices +
   `TISRegisterInputSource`, and drops a one-shot **LaunchAgent**.
2. **Log out & back in** — macOS only enrolls a new input method during the
   login scan. The **Log Out Now** button drives `System Events` to log out
   (one-time Automation prompt; see `NSAppleEventsUsageDescription`).
3. **Activate (post-login)** — at the next login the LaunchAgent relaunches the
   installer with `--post-login`; it `TISEnableInputSource` + `TISSelectInputSource`
   the now-registered source, opens **Keyboard ▸ Input Sources**, then removes the
   LaunchAgent and its stable copy.

The installer copies itself to
`~/Library/Application Support/DSInput/DSInputSetup.app` so the LaunchAgent
survives the user deleting the original (e.g. emptying `~/Downloads`).

## Build

```bash
bash macos/Installer/build-installer.sh        # → macos/Installer/build/DSInputInstaller.app
```

This builds the IME (`macos/build.sh`, Developer-ID signed — notarization
skipped for local use), builds the installer with `xcodebuild`, embeds
`DSInput.app` into its `Resources/`, and Developer-ID signs the result. For
distribution to other machines, also notarize the IME and the installer.

## Notes

- The installer is a normal app (`io.github.madeye.dsinput.installer`); it does
  **not** need the IME's `.inputmethod.` id rules.
- The embedded `DSInput.app` keeps its own signature (the installer is signed
  non-`--deep`), so it stays valid when copied into `~/Library/Input Methods`.
