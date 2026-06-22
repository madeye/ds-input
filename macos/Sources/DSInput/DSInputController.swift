/*
 * DSInputController.swift — IMKInputController subclass.
 *
 * One instance is created per client application by InputMethodKit.
 * It owns one DsSession for the process lifetime of that client slot.
 *
 * Composition state machine
 * ─────────────────────────
 *   pinyinBuffer   : the raw ASCII the user has typed so far (a–z, ')
 *   preEditText    : what we show as marked/underlined text
 *                    = pinyinBuffer while no result yet, or last Chinese result
 *   latestRequestId: monotonic counter to discard stale callbacks
 *
 * Key handling
 * ────────────
 *   a–z / '        → append to pinyinBuffer, show raw pinyin as pre-edit,
 *                    schedule debounced conversion
 *   Backspace       → pop last char from pinyinBuffer, reschedule conversion
 *   Space           → 分词: first Space appends a word-boundary space and
 *                    re-converts; a DOUBLE space (two in a row) commits — the
 *                    converted sentence, else the raw pinyin (boundaries
 *                    trimmed). Mirrors the Windows TSF frontend.
 *   Return          → commit current pre-edit, reset session
 *   Escape          → revert pre-edit to raw pinyin (cancel conversion),
 *                    or if already showing pinyin, clear entirely
 *   Any other key  → pass through if no active composition; else commit + pass
 */

import AppKit
import InputMethodKit

// MARK: - C callback (must be a top-level @convention(c) function)

/// Streaming result callback, called by the Rust core on a background thread.
/// Fires with isFinal=0 for each partial (cumulative) pre-edit update, then once
/// with isFinal=1 for the terminal outcome. We hop to the main queue to apply.
private func convertStreamCallback(
    userData: UnsafeMutableRawPointer?,
    requestId: UInt64,
    status: Int32,
    isFinal: Int32,
    textUtf8: UnsafePointer<CChar>?
) {
    guard let userData = userData else { return }

    // We used Unmanaged.passRetained when scheduling the conversion. Per dsime.h
    // the retain is tied to the TERMINAL call: balance it only when isFinal=1,
    // and merely borrow it on partial updates.
    let unmanaged = Unmanaged<DSInputController>.fromOpaque(userData)
    let controller = isFinal != 0
        ? unmanaged.takeRetainedValue()
        : unmanaged.takeUnretainedValue()

    // Copy the C string before the callback returns (text_utf8 is only valid
    // during the call, per dsime.h).
    let text: String? = textUtf8.map { String(cString: $0) }

    DispatchQueue.main.async {
        controller.handleConversionResult(requestId: requestId, status: status, text: text)
    }
}

// MARK: - DSInputController

// @objc(...) pins the Objective-C runtime name so it matches
// InputMethodServerControllerClass in Info.plist. Without this, a Swift class's
// runtime name is module-prefixed (DSInput.DSInputController) and IMK can't find it.
@objc(DSInputController)
final class DSInputController: IMKInputController {

    // MARK: Session

    /// Per-controller Rust session.
    private var session: OpaquePointer? // DsSession *

    // MARK: Composition state

    /// Raw pinyin accumulated from keystrokes.
    private var pinyinBuffer: String = ""

    /// Text currently shown as the marked/pre-edit string.
    /// nil means no active composition.
    private var preEditText: String? = nil

    /// Monotonic counter we assign to each conversion request.
    /// The callback checks this to discard stale results.
    private var latestRequestId: UInt64 = 0

    // MARK: Debounce

    private var debounceTimer: Timer?

    /// Idle interval before we fire ds_session_convert.
    private var debounceDuration: TimeInterval {
        guard let engine = sharedDsEngine else { return 0.10 }
        let ms = ds_engine_debounce_ms(engine)
        return Double(ms) / 1000.0
    }

    // MARK: - Lifecycle

    override init!(server: IMKServer!, delegate: Any!, client: Any!) {
        super.init(server: server, delegate: delegate, client: client)
        guard let engine = sharedDsEngine else {
            NSLog("[DSInput] Controller init: no engine")
            return nil
        }
        session = ds_session_new(engine)
        if session == nil {
            NSLog("[DSInput] Controller init: ds_session_new failed: \(String(cString: ds_last_error()))")
            return nil
        }
    }

    deinit {
        debounceTimer?.invalidate()
        if let s = session {
            ds_session_free(s)
        }
    }

    // MARK: - Key handling

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard event.type == .keyDown else { return false }

        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let hasModifier = !flags.isEmpty && flags != .capsLock

        // Let modified keystrokes (⌘, ⌃, ⌥) pass through.
        if hasModifier { return commitAndPassThrough(event: event, client: sender) }

        let keyCode = event.keyCode
        let chars = event.characters ?? ""

        switch keyCode {
        case 0x33: // Backspace (kVK_Delete)
            return handleBackspace(client: sender)

        case 0x24: // Return (kVK_Return)
            return handleCommit(client: sender)

        case 0x31: // Space (kVK_Space)
            return handleSpace(client: sender)

        case 0x35: // Escape (kVK_Escape)
            return handleEscape(client: sender)

        default:
            if let scalar = chars.unicodeScalars.first {
                // Accept printable ASCII pinyin input: a–z and apostrophe.
                if (scalar.value >= 0x61 && scalar.value <= 0x7A) || scalar == "'" {
                    return handlePinyinChar(String(scalar), client: sender)
                }
                // Full-width punctuation (全角): emit the mapped symbol.
                if let full = Self.fullWidthPunct(scalar) {
                    return handlePunctuation(full, client: sender)
                }
            }
            // Any other key: commit if composing, then let the keystroke through.
            return commitAndPassThrough(event: event, client: sender)
        }
    }

    // MARK: - Key handlers

    private func handlePinyinChar(_ char: String, client sender: Any!) -> Bool {
        // Context cap: if the uncommitted buffer is already at the token budget,
        // flush it (commit the current pre-edit — Chinese if shown, else raw
        // pinyin) and start fresh so the next request stays small. The char being
        // typed begins the new buffer.
        if !pinyinBuffer.isEmpty, let s = session, ds_session_context_full(s) != 0 {
            let flush = preEditText ?? pinyinBuffer
            if !flush.isEmpty { commit(flush, client: sender) }
        }
        pinyinBuffer.append(char)
        updateSessionInput()
        showPreEdit(pinyinBuffer, client: sender) // show raw pinyin immediately
        scheduleDebounce(client: sender)
        return true
    }

    private func handlePunctuation(_ full: String, client sender: Any!) -> Bool {
        // Output the full-width (全角) symbol. If composing, commit the current
        // pre-edit first with the symbol appended so the Chinese sentence and its
        // punctuation land together; otherwise insert the symbol directly.
        if let text = preEditText, !text.isEmpty {
            commit(text + full, client: sender)
        } else if let client = sender as? (any IMKTextInput & NSObjectProtocol) {
            client.insertText(full, replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        return true
    }

    /// Map an ASCII punctuation scalar to its full-width (全角) equivalent, or nil.
    private static func fullWidthPunct(_ s: Unicode.Scalar) -> String? {
        switch s.value {
        case 0x2C: return "，"  // ,
        case 0x2E: return "。"  // .
        case 0x3F: return "？"  // ?
        case 0x21: return "！"  // !
        case 0x3B: return "；"  // ;
        case 0x3A: return "："  // :
        case 0x28: return "（"  // (
        case 0x29: return "）"  // )
        case 0x5C: return "、"  // backslash
        default: return nil
        }
    }

    private func handleBackspace(client sender: Any!) -> Bool {
        guard !pinyinBuffer.isEmpty else {
            // Nothing to delete; pass the key through.
            return false
        }
        pinyinBuffer.removeLast()
        if pinyinBuffer.isEmpty {
            clearComposition(client: sender)
        } else {
            updateSessionInput()
            showPreEdit(pinyinBuffer, client: sender)
            scheduleDebounce(client: sender)
        }
        return true
    }

    /// Space does 分词 (word segmentation), matching the Windows TSF frontend:
    /// the first Space appends a word-boundary space to the pinyin buffer and
    /// re-converts; a DOUBLE space (two consecutive) confirms/commits. We detect
    /// the second of two spaces by a trailing space already in the buffer.
    private func handleSpace(client sender: Any!) -> Bool {
        // With an empty buffer there's nothing to segment; let Space through.
        guard !pinyinBuffer.isEmpty else { return false }

        if pinyinBuffer.hasSuffix(" ") {
            // Second consecutive space -> commit. Prefer the converted sentence;
            // otherwise fall back to the raw pinyin with boundary spaces trimmed
            // so text is always output, even with no/slow conversion.
            let committed: String
            if let text = preEditText, text != pinyinBuffer {
                committed = text // converted Chinese
            } else {
                var trimmed = pinyinBuffer
                while trimmed.hasSuffix(" ") { trimmed.removeLast() }
                committed = trimmed
            }
            commit(committed, client: sender)
            return true
        }

        // First space: append a 分词 boundary and re-arm auto-conversion. Keep
        // showing the current conversion (if any) until the new result lands — a
        // trailing boundary space doesn't change the sentence, so a quick
        // double-space still commits the Chinese rather than the pinyin.
        let wasConverted = preEditText != nil && preEditText != pinyinBuffer
        pinyinBuffer.append(" ")
        updateSessionInput()
        if !wasConverted {
            showPreEdit(pinyinBuffer, client: sender) // raw pinyin with boundary
        }
        scheduleDebounce(client: sender)
        return true
    }

    private func handleCommit(client sender: Any!) -> Bool {
        guard let text = preEditText, !text.isEmpty else {
            return false
        }
        commit(text, client: sender)
        return true
    }

    private func handleEscape(client sender: Any!) -> Bool {
        guard preEditText != nil else { return false }

        debounceTimer?.invalidate()
        debounceTimer = nil

        if let s = session { ds_session_cancel(s) }

        if preEditText != pinyinBuffer {
            // We were showing Chinese; revert to raw pinyin.
            preEditText = pinyinBuffer
            showPreEdit(pinyinBuffer, client: sender)
        } else {
            // Already showing raw pinyin; clear entirely.
            clearComposition(client: sender)
        }
        return true
    }

    /// Commit current pre-edit (if any) then pass the original event through.
    private func commitAndPassThrough(event: NSEvent!, client sender: Any!) -> Bool {
        if let text = preEditText, !text.isEmpty {
            commit(text, client: sender)
        }
        return false // let IMK forward the event
    }

    // MARK: - Debounce & conversion

    private func scheduleDebounce(client sender: Any!) {
        debounceTimer?.invalidate()
        let interval = debounceDuration
        debounceTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: false) { [weak self] _ in
            self?.triggerConversion(client: sender)
        }
    }

    private func triggerConversion(client _: Any!) {
        guard let s = session, !pinyinBuffer.isEmpty else { return }

        // Bump our local counter; use it as a generation number.
        // The C layer returns its own request_id — we track ours separately
        // to guard against any race where a very old callback fires.
        latestRequestId += 1
        let expectedId = latestRequestId

        // Pass self as userData via Unmanaged retain; the terminal (is_final=1)
        // callback releases it. Streaming fills the pre-edit token-by-token.
        let retained = Unmanaged.passRetained(self).toOpaque()
        let cRequestId = ds_session_convert_stream(s, convertStreamCallback, retained)

        // If the buffer was empty ds_session_convert_stream returns 0 and the
        // callback never fires, so we'd leak the retain.  Guard that case.
        if cRequestId == 0 {
            _ = Unmanaged<DSInputController>.fromOpaque(retained).takeRetainedValue()
        }

        // Store both ids so handleConversionResult can validate.
        _ = expectedId // used below via closure capture in handleConversionResult
        NSLog("[DSInput] Conversion triggered, c_request_id=\(cRequestId), local_gen=\(expectedId)")
    }

    // MARK: - Callback entry point (main thread)

    func handleConversionResult(requestId: UInt64, status: Int32, text: String?) {
        // Ignore results that arrived after the user already committed / cleared.
        guard preEditText != nil else { return }

        if status == DS_OK, let chinese = text, !chinese.isEmpty {
            preEditText = chinese
            // Re-show the updated pre-edit in the active client.
            // We need the client object; retrieve it from IMK.
            if let client = self.client() {
                showPreEdit(chinese, client: client)
            }
        } else if status != DS_ERR_CANCELLED {
            let msg = text ?? String(cString: ds_last_error())
            NSLog("[DSInput] Conversion error (status=\(status)): \(msg)")
            // Keep showing raw pinyin; no state change needed.
        }
    }

    // MARK: - IMK composition helpers

    private func updateSessionInput() {
        guard let s = session else { return }
        pinyinBuffer.withCString { ds_session_set_input(s, $0) }
    }

    private func showPreEdit(_ text: String, client sender: Any!) {
        preEditText = text
        guard let client = sender as? (any IMKTextInput & NSObjectProtocol) else { return }

        // Build a marked string with a single underline attribute range.
        let attrs = mark(forStyle: kTSMHiliteSelectedConvertedText, at: NSRange(location: NSNotFound, length: 0))
        let marked = NSAttributedString(
            string: text,
            attributes: attrs as? [NSAttributedString.Key: Any] ?? [:]
        )
        client.setMarkedText(marked, selectionRange: NSRange(location: text.utf16.count, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
    }

    private func commit(_ text: String, client sender: Any!) {
        // 1. Clear the marked text.
        if let client = sender as? (any IMKTextInput & NSObjectProtocol) {
            client.setMarkedText("", selectionRange: NSRange(location: 0, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        // 2. Insert the committed text.
        if let client = sender as? (any IMKTextInput & NSObjectProtocol) {
            client.insertText(text, replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        // 3. Reset session state.
        debounceTimer?.invalidate()
        debounceTimer = nil
        pinyinBuffer = ""
        preEditText = nil
        latestRequestId = 0
        if let s = session { ds_session_reset(s) }
    }

    private func clearComposition(client sender: Any!) {
        if let client = sender as? (any IMKTextInput & NSObjectProtocol) {
            client.setMarkedText("", selectionRange: NSRange(location: 0, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        debounceTimer?.invalidate()
        debounceTimer = nil
        pinyinBuffer = ""
        preEditText = nil
        latestRequestId = 0
        if let s = session { ds_session_reset(s) }
    }

    // MARK: - IMK lifecycle callbacks

    override func activateServer(_ sender: Any!) {
        // Nothing special needed; session is ready.
    }

    override func deactivateServer(_ sender: Any!) {
        // Commit any pending composition on focus loss.
        if let text = preEditText, !text.isEmpty {
            commit(text, client: sender)
        }
    }

    // MARK: - IMK menu

    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "DS Input")
        menu.addItem(withTitle: "Preferences…", action: #selector(openPreferences), keyEquivalent: ",")
        menu.addItem(NSMenuItem.separator())
        let ver = String(cString: ds_version())
        menu.addItem(withTitle: "Core v\(ver)", action: nil, keyEquivalent: "")
        return menu
    }

    @objc private func openPreferences() {
        PreferencesWindowController.shared.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}
