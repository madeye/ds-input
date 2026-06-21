//
//  InstallerModel.swift — state machine + system actions for the DS Input
//  per-user installer (a WeType-style guided installer).
//
//  Flow:
//    welcome → installing → readyToLogout → (auto log out) → [re-login] →
//    enabling → finished
//
//  The installer bundles DSInput.app in its Resources and copies it into
//  ~/Library/Input Methods (no admin needed). Because a brand-new input source
//  is only enrolled by the system's login-time scan, we install a per-user
//  LaunchAgent that relaunches this app with `--post-login` after the user logs
//  back in; that pass enables + selects the input source and opens Settings.
//

import AppKit
import Carbon
import Combine

enum InstallStep: Equatable {
    case welcome
    case installing
    case readyToLogout
    case enabling
    case finished
    case failed(String)
}

final class InstallerModel: ObservableObject {
    @Published private(set) var step: InstallStep
    @Published private(set) var progressText: String = ""

    // MARK: Identifiers (must match the IME bundle)
    let imeAppName = "DSInput.app"
    let imeSourceID = "io.github.madeye.inputmethod.dsinput.pinyin"
    let helperLabel = "io.github.madeye.inputmethod.dsinput.setup"

    // MARK: Paths
    private let fm = FileManager.default
    private var home: URL { fm.homeDirectoryForCurrentUser }
    var inputMethodsDir: URL { home.appendingPathComponent("Library/Input Methods") }
    var installedIMEURL: URL { inputMethodsDir.appendingPathComponent(imeAppName) }
    private var appSupportDir: URL { home.appendingPathComponent("Library/Application Support/DSInput") }
    /// A stable copy of this installer so the LaunchAgent survives the user
    /// moving/deleting the original (e.g. emptying ~/Downloads).
    private var stableSetupAppURL: URL { appSupportDir.appendingPathComponent("DSInputSetup.app") }
    private var launchAgentsDir: URL { home.appendingPathComponent("Library/LaunchAgents") }
    private var helperPlistURL: URL { launchAgentsDir.appendingPathComponent("\(helperLabel).plist") }
    /// DSInput.app bundled inside this installer's Resources.
    private var bundledIMEURL: URL? { Bundle.main.url(forResource: "DSInput", withExtension: "app") }

    init(postLogin: Bool) {
        step = postLogin ? .enabling : .welcome
        if postLogin {
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in self?.runPostLogin() }
        }
    }

    // MARK: - Step 1 → 2: install the bundle

    func install() {
        set(step: .installing)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            do {
                try self.performInstall()
                self.set(step: .readyToLogout)
            } catch {
                self.set(step: .failed(error.localizedDescription))
            }
        }
    }

    private func performInstall() throws {
        guard let src = bundledIMEURL else {
            throw InstallError("DSInput.app is missing from the installer bundle.")
        }
        set(progress: "Copying DS Input to your Input Methods folder…")
        try fm.createDirectory(at: inputMethodsDir, withIntermediateDirectories: true)
        if fm.fileExists(atPath: installedIMEURL.path) {
            try fm.removeItem(at: installedIMEURL)
        }
        try fm.copyItem(at: src, to: installedIMEURL)

        set(progress: "Registering with the system…")
        runLSRegister(installedIMEURL)
        TISRegisterInputSource(installedIMEURL as CFURL)

        set(progress: "Setting up the post-login helper…")
        try installLoginHelper()
    }

    /// Copy this installer to a stable location and write a LaunchAgent that
    /// relaunches it `--post-login` at the next login (when the input source
    /// has been enrolled by the system scan).
    private func installLoginHelper() throws {
        try fm.createDirectory(at: appSupportDir, withIntermediateDirectories: true)
        try fm.createDirectory(at: launchAgentsDir, withIntermediateDirectories: true)

        if fm.fileExists(atPath: stableSetupAppURL.path) {
            try fm.removeItem(at: stableSetupAppURL)
        }
        try fm.copyItem(at: Bundle.main.bundleURL, to: stableSetupAppURL)

        let execName = Bundle.main.executableURL?.lastPathComponent ?? "DSInputInstaller"
        let execPath = stableSetupAppURL
            .appendingPathComponent("Contents/MacOS")
            .appendingPathComponent(execName).path

        let plist: [String: Any] = [
            "Label": helperLabel,
            "ProgramArguments": [execPath, "--post-login"],
            "RunAtLoad": true,
            "LimitLoadToSessionType": "Aqua",
        ]
        let data = try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
        try data.write(to: helperPlistURL)
        // launchd loads ~/Library/LaunchAgents at the next login automatically.
    }

    // MARK: - Auto log out

    func logOutNow() {
        // Ask System Events to log out — shows the standard confirmation, then
        // ends the session. Triggers a one-time Automation prompt (described by
        // NSAppleEventsUsageDescription).
        let script = "tell application \"System Events\" to log out"
        var err: NSDictionary?
        NSAppleScript(source: script)?.executeAndReturnError(&err)
        if let err = err { NSLog("[DSInputInstaller] log out failed: \(err)") }
    }

    // MARK: - Step 4 (post-login): enable + select + open Settings

    private func runPostLogin() {
        // Give the login-time input-source scan a moment to settle.
        Thread.sleep(forTimeInterval: 1.5)

        var enabled = false
        for attempt in 0..<10 {
            if let src = Self.findInputSource(imeSourceID) {
                TISEnableInputSource(src)
                TISSelectInputSource(src)
                enabled = true
                break
            }
            set(progress: "Waiting for the system to register DS Input… (\(attempt + 1))")
            Thread.sleep(forTimeInterval: 0.7)
        }

        DispatchQueue.main.async { self.openInputSourcesSettings() }
        cleanupLoginHelper()
        if !enabled {
            set(progress: "DS Input is installed. If it isn't active yet, add it under "
                + "System Settings ▸ Keyboard ▸ Input Sources ▸ + ▸ Simplified Chinese.")
        }
        set(step: .finished)
    }

    private func openInputSourcesSettings() {
        let url = URL(string: "x-apple.systempreferences:com.apple.Keyboard-Settings.extension")!
        NSWorkspace.shared.open(url)
    }

    /// One-shot helper cleanup: remove the LaunchAgent and the stable copy.
    private func cleanupLoginHelper() {
        _ = try? runProcess("/bin/launchctl", ["bootout", "gui/\(getuid())/\(helperLabel)"])
        try? fm.removeItem(at: helperPlistURL)
        try? fm.removeItem(at: stableSetupAppURL)
    }

    // MARK: - TIS helpers

    static func findInputSource(_ id: String) -> TISInputSource? {
        guard let cf = TISCreateInputSourceList(nil, true)?.takeRetainedValue(),
              let list = cf as? [TISInputSource] else { return nil }
        for src in list {
            guard let p = TISGetInputSourceProperty(src, kTISPropertyInputSourceID) else { continue }
            let sid = Unmanaged<CFString>.fromOpaque(p).takeUnretainedValue() as String
            if sid == id { return src }
        }
        return nil
    }

    // MARK: - Process helpers

    private func runLSRegister(_ url: URL) {
        let lsregister = "/System/Library/Frameworks/CoreServices.framework/Versions/A/"
            + "Frameworks/LaunchServices.framework/Support/lsregister"
        _ = try? runProcess(lsregister, ["-f", url.path])
    }

    @discardableResult
    private func runProcess(_ launchPath: String, _ args: [String]) throws -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: launchPath)
        p.arguments = args
        try p.run()
        p.waitUntilExit()
        return p.terminationStatus
    }

    // MARK: - Published updates (always on main)

    private func set(step newStep: InstallStep) {
        DispatchQueue.main.async { self.step = newStep }
    }
    private func set(progress text: String) {
        DispatchQueue.main.async { self.progressText = text }
    }
}

struct InstallError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
