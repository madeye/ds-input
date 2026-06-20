/*
 * AppDelegate.swift — application entry point for DS Input IME.
 *
 * Responsibilities:
 *   1. Start the IMKServer so InputMethodKit routes key events to our controller.
 *   2. Create and hold the single shared DsEngine for the process lifetime.
 *   3. Offer a Settings menu item that opens the preferences window.
 *
 * The bundle connection name must match the Info.plist key
 * InputMethodConnectionName (= "DSInput_Connection").
 */

import AppKit
import InputMethodKit

// MARK: - Engine singleton

/// Lazily created process-wide engine. All IMKInputController instances share it.
/// Initialised once in applicationDidFinishLaunching; never freed (process lifetime).
var sharedDsEngine: OpaquePointer? = nil // DsEngine *

// MARK: - AppDelegate

// NOTE: entry point lives in main.swift, which constructs NSApplication and
// wires this delegate explicitly. We must NOT use @main here: with
// NSPrincipalClass=NSApplication and no nib, @main never connects the delegate,
// so applicationDidFinishLaunching would never fire.
final class AppDelegate: NSObject, NSApplicationDelegate {

    /// IMKServer keeps the input method alive and routes events.
    private var imkServer: IMKServer?

    /// Menubar presence so the user can open Preferences any time — including
    /// before the input source is registered (which needs a login cycle).
    private var statusItem: NSStatusItem?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 1. Initialise the core engine (nil config_path → per-user default).
        sharedDsEngine = ds_engine_new(nil)
        if sharedDsEngine == nil {
            let err = String(cString: ds_last_error())
            NSLog("[DSInput] Fatal: ds_engine_new failed: \(err)")
            NSApp.terminate(nil)
            return
        }
        let ver = String(cString: ds_version())
        NSLog("[DSInput] Core engine v\(ver) ready")

        // 2. Start the IMKServer.  The connection name must match Info.plist.
        let connectionName = Bundle.main.object(forInfoDictionaryKey: "InputMethodConnectionName") as? String ?? "DSInput_Connection"
        imkServer = IMKServer(name: connectionName, bundleIdentifier: Bundle.main.bundleIdentifier)
        if imkServer == nil {
            NSLog("[DSInput] Fatal: IMKServer init failed for connection '\(connectionName)'")
            NSApp.terminate(nil)
            return
        }
        NSLog("[DSInput] IMKServer started on '\(connectionName)'")

        // 3. Menubar item: a small "拼" title with a Preferences / Quit menu, so
        //    settings are reachable without the IME being active.
        setupStatusItem()

        // 4. First-run convenience: if no API key is configured yet, pop the
        //    Preferences window so the user can set the URL + key immediately.
        if !hasApiKey() {
            DispatchQueue.main.async { [weak self] in
                self?.openPreferences()
            }
        }
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "拼"
        item.button?.toolTip = "DS Input (LLM Pinyin)"
        let menu = NSMenu()
        let prefs = menu.addItem(withTitle: "DS Input Preferences…",
                                 action: #selector(openPreferences), keyEquivalent: ",")
        prefs.target = self
        menu.addItem(.separator())
        let ver = String(cString: ds_version())
        menu.addItem(withTitle: "Core v\(ver)", action: nil, keyEquivalent: "")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit DS Input",
                     action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.menu = menu
        statusItem = item
    }

    /// True if the persisted config already has a non-empty api_key.
    private func hasApiKey() -> Bool {
        guard let engine = sharedDsEngine, let cjson = ds_engine_get_config_json(engine) else {
            return false
        }
        defer { ds_string_free(cjson) }
        let json = String(cString: cjson)
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let key = obj["api_key"] as? String else { return false }
        return !key.isEmpty
    }

    @objc private func openPreferences() {
        PreferencesWindowController.shared.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        PreferencesWindowController.shared.window?.makeKeyAndOrderFront(nil)
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let engine = sharedDsEngine {
            ds_engine_free(engine)
            sharedDsEngine = nil
        }
    }
}
