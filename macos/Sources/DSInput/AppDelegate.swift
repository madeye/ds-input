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

@main
final class AppDelegate: NSObject, NSApplicationDelegate {

    /// IMKServer keeps the input method alive and routes events.
    private var imkServer: IMKServer?

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
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let engine = sharedDsEngine {
            ds_engine_free(engine)
            sharedDsEngine = nil
        }
    }
}
