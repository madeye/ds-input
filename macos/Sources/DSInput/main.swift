/*
 * main.swift — explicit entry point for DS Input.
 *
 * An InputMethodKit app must construct NSApplication, attach its delegate, and
 * run the loop itself. We deliberately do NOT use @main on AppDelegate: with
 * NSPrincipalClass=NSApplication and no nib, @main never connects the delegate,
 * so applicationDidFinishLaunching (which starts the engine + IMKServer) would
 * never run. Wiring it here guarantees the lifecycle fires whether the app is
 * launched by the input system or by the user opening it to reach Settings.
 */

import AppKit

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
// Accessory: no Dock icon (matches LSUIElement), but status-bar items and
// windows (the Preferences panel) still display and can be activated.
app.setActivationPolicy(.accessory)
app.run()
