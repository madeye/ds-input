/*
 * PreferencesWindowController.swift — Settings UI for DS Input.
 *
 * Displays a simple AppKit form backed by ds_engine_get/set_config_json.
 * Fields: Base URL, API Key (secure), Model, Temperature, Max Tokens,
 *         Timeout (ms), Debounce (ms), System Prompt (multi-line).
 *
 * JSON schema expected by the core (Config struct):
 *   {
 *     "base_url":       string,
 *     "api_key":        string,
 *     "model":          string,
 *     "system_prompt":  string,
 *     "temperature":    number,
 *     "max_tokens":     integer,
 *     "timeout_ms":     integer,
 *     "debounce_ms":    integer
 *   }
 */

import AppKit

// MARK: - Codable config mirror

private struct DsConfig: Codable {
    var base_url: String
    var api_key: String
    var model: String
    var system_prompt: String
    var temperature: Double
    var max_tokens: Int
    var timeout_ms: UInt32
    var debounce_ms: UInt32
}

// MARK: - PreferencesWindowController

final class PreferencesWindowController: NSWindowController, NSWindowDelegate {

    static let shared = PreferencesWindowController()

    // MARK: Form fields

    private var baseUrlField: NSTextField!
    private var apiKeyField: NSSecureTextField!
    private var modelField: NSTextField!
    private var temperatureField: NSTextField!
    private var maxTokensField: NSTextField!
    private var timeoutField: NSTextField!
    private var debounceField: NSTextField!
    private var systemPromptView: NSTextView!
    private var statusLabel: NSTextField!

    // MARK: - Init

    private init() {
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 520, height: 480),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        win.title = "DS Input Preferences"
        win.center()
        super.init(window: win)
        win.delegate = self
        buildUI()
    }

    required init?(coder: NSCoder) { fatalError("not implemented") }

    // MARK: - UI construction

    private func buildUI() {
        guard let contentView = window?.contentView else { return }
        contentView.wantsLayer = true

        // Labels + fields layout (manual frames for simplicity)
        let labelWidth: CGFloat = 130
        let fieldX: CGFloat = 145
        let fieldWidth: CGFloat = 350
        let rowHeight: CGFloat = 24
        let rowGap: CGFloat = 8
        let startY: CGFloat = 440

        func makeLabel(_ text: String, y: CGFloat) -> NSTextField {
            let l = NSTextField(labelWithString: text)
            l.alignment = .right
            l.frame = NSRect(x: 10, y: y, width: labelWidth, height: rowHeight)
            return l
        }

        func makeField(y: CGFloat, secure: Bool = false) -> NSTextField {
            let f: NSTextField = secure ? NSSecureTextField() : NSTextField()
            f.frame = NSRect(x: fieldX, y: y, width: fieldWidth, height: rowHeight)
            f.bezelStyle = .roundedBezel
            return f
        }

        var y = startY

        // Base URL
        contentView.addSubview(makeLabel("Base URL:", y: y))
        baseUrlField = makeField(y: y)
        contentView.addSubview(baseUrlField)
        y -= rowHeight + rowGap

        // API Key
        contentView.addSubview(makeLabel("API Key:", y: y))
        apiKeyField = NSSecureTextField(frame: NSRect(x: fieldX, y: y, width: fieldWidth, height: rowHeight))
        apiKeyField.bezelStyle = .roundedBezel
        contentView.addSubview(apiKeyField)
        y -= rowHeight + rowGap

        // Model
        contentView.addSubview(makeLabel("Model:", y: y))
        modelField = makeField(y: y)
        contentView.addSubview(modelField)
        y -= rowHeight + rowGap

        // Temperature
        contentView.addSubview(makeLabel("Temperature:", y: y))
        temperatureField = makeField(y: y)
        contentView.addSubview(temperatureField)
        y -= rowHeight + rowGap

        // Max Tokens
        contentView.addSubview(makeLabel("Max Tokens:", y: y))
        maxTokensField = makeField(y: y)
        contentView.addSubview(maxTokensField)
        y -= rowHeight + rowGap

        // Timeout ms
        contentView.addSubview(makeLabel("Timeout (ms):", y: y))
        timeoutField = makeField(y: y)
        contentView.addSubview(timeoutField)
        y -= rowHeight + rowGap

        // Debounce ms
        contentView.addSubview(makeLabel("Debounce (ms):", y: y))
        debounceField = makeField(y: y)
        contentView.addSubview(debounceField)
        y -= rowHeight + rowGap * 2

        // System Prompt (multi-line)
        contentView.addSubview(makeLabel("System Prompt:", y: y - 30))
        let scrollView = NSScrollView(frame: NSRect(x: fieldX, y: y - 80, width: fieldWidth, height: 90))
        scrollView.hasVerticalScroller = true
        scrollView.borderType = .bezelBorder
        systemPromptView = NSTextView(frame: scrollView.bounds)
        systemPromptView.isEditable = true
        systemPromptView.isRichText = false
        systemPromptView.font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        scrollView.documentView = systemPromptView
        contentView.addSubview(scrollView)
        y -= 90 + rowGap * 2

        // Status label
        statusLabel = NSTextField(labelWithString: "")
        statusLabel.frame = NSRect(x: fieldX, y: y, width: fieldWidth, height: rowHeight)
        statusLabel.textColor = .secondaryLabelColor
        contentView.addSubview(statusLabel)
        y -= rowHeight + rowGap

        // Buttons
        let saveBtn = NSButton(title: "Save", target: self, action: #selector(save))
        saveBtn.bezelStyle = .rounded
        saveBtn.frame = NSRect(x: fieldX + fieldWidth - 80, y: 16, width: 80, height: 28)
        saveBtn.keyEquivalent = "\r"
        contentView.addSubview(saveBtn)

        let reloadBtn = NSButton(title: "Reload from Disk", target: self, action: #selector(reload))
        reloadBtn.bezelStyle = .rounded
        reloadBtn.frame = NSRect(x: fieldX, y: 16, width: 140, height: 28)
        contentView.addSubview(reloadBtn)
    }

    // MARK: - Window show

    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        loadFromEngine()
    }

    // MARK: - Load / save

    private func loadFromEngine() {
        guard let engine = sharedDsEngine else { return }
        guard let cjson = ds_engine_get_config_json(engine) else {
            statusLabel.stringValue = "Could not read config."
            return
        }
        let jsonStr = String(cString: cjson)
        ds_string_free(cjson)

        guard let data = jsonStr.data(using: .utf8),
              let cfg = try? JSONDecoder().decode(DsConfig.self, from: data) else {
            statusLabel.stringValue = "Config JSON parse error."
            return
        }

        baseUrlField.stringValue    = cfg.base_url
        apiKeyField.stringValue     = cfg.api_key
        modelField.stringValue      = cfg.model
        temperatureField.stringValue = String(cfg.temperature)
        maxTokensField.stringValue  = String(cfg.max_tokens)
        timeoutField.stringValue    = String(cfg.timeout_ms)
        debounceField.stringValue   = String(cfg.debounce_ms)
        systemPromptView.string     = cfg.system_prompt
        statusLabel.stringValue     = "Loaded."
    }

    @objc private func reload() {
        guard let engine = sharedDsEngine else { return }
        let status = ds_engine_reload_config(engine)
        if status == DS_OK {
            loadFromEngine()
            statusLabel.stringValue = "Reloaded from disk."
        } else {
            statusLabel.stringValue = "Reload failed: \(String(cString: ds_last_error()))"
        }
    }

    @objc private func save() {
        guard let engine = sharedDsEngine else { return }

        let temperature = Double(temperatureField.stringValue) ?? 0.3
        let maxTokens   = Int(maxTokensField.stringValue) ?? 512
        let timeoutMs   = UInt32(timeoutField.stringValue) ?? 10000
        let debounceMs  = UInt32(debounceField.stringValue) ?? 180

        let cfg = DsConfig(
            base_url:      baseUrlField.stringValue,
            api_key:       apiKeyField.stringValue,
            model:         modelField.stringValue,
            system_prompt: systemPromptView.string,
            temperature:   temperature,
            max_tokens:    maxTokens,
            timeout_ms:    timeoutMs,
            debounce_ms:   debounceMs
        )

        guard let data = try? JSONEncoder().encode(cfg),
              let jsonStr = String(data: data, encoding: .utf8) else {
            statusLabel.stringValue = "Failed to encode config."
            return
        }

        let status = jsonStr.withCString { ds_engine_set_config_json(engine, $0) }
        if status == DS_OK {
            statusLabel.stringValue = "Settings saved."
        } else {
            statusLabel.stringValue = "Save failed: \(String(cString: ds_last_error()))"
        }
    }

    // MARK: - NSWindowDelegate

    func windowWillClose(_ notification: Notification) {
        // Nothing needed; window is reused.
    }
}
