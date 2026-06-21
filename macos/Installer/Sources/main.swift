//
//  main.swift — entry point for the DS Input installer.
//
//  Launched two ways:
//    • normally (user double-clicks)            → starts at the Welcome step.
//    • by the post-login LaunchAgent (--post-login) → activates the input source.
//

import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private let model: InstallerModel

    init(postLogin: Bool) {
        model = InstallerModel(postLogin: postLogin)
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let hosting = NSHostingController(rootView: WizardView(model: model))
        window = NSWindow(contentViewController: hosting)
        window.title = "Install DS Input"
        window.styleMask = [.titled, .closable, .miniaturizable]
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let postLogin = CommandLine.arguments.contains("--post-login")
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate(postLogin: postLogin)
app.delegate = delegate
app.run()
