//
//  WizardView.swift — the guided, step-by-step installer UI.
//

import SwiftUI

struct WizardView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            Group {
                switch model.step {
                case .welcome: WelcomeStep(model: model)
                case .installing: ProgressStep(title: "Installing…", model: model)
                case .readyToLogout: LogoutStep(model: model)
                case .enabling: ProgressStep(title: "Finishing setup…", model: model)
                case .finished: FinishedStep(model: model)
                case .failed(let msg): FailedStep(model: model, message: msg)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(28)
        }
        .frame(width: 560, height: 460)
    }

    private var header: some View {
        HStack(spacing: 14) {
            Image(systemName: "character.cursor.ibeam")
                .font(.system(size: 30, weight: .bold))
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text("DS Input").font(.title2).bold()
                Text("LLM whole-sentence pinyin input method").font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            StepDots(current: model.step)
        }
        .padding(.horizontal, 24).padding(.vertical, 16)
    }
}

// MARK: - Steps

private struct WelcomeStep: View {
    @ObservedObject var model: InstallerModel
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("Welcome").font(.title).bold()
            Text("This installer sets up DS Input for your account. You type toneless "
                + "pinyin and an LLM converts the whole sentence to Chinese inline — no "
                + "candidate window.")
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 14) {
                StepRow(n: 1, title: "Install",
                        detail: "Copy DS Input into your personal Input Methods folder. No password needed.")
                StepRow(n: 2, title: "Log out & back in",
                        detail: "macOS only registers a new input method during login. We can log you out for this.")
                StepRow(n: 3, title: "Activate",
                        detail: "After you log back in, we enable DS Input and open Keyboard settings automatically.")
            }
            Spacer()
            HStack {
                Spacer()
                Button("Install DS Input") { model.install() }
                    .keyboardShortcut(.defaultAction)
                    .controlSize(.large)
            }
        }
    }
}

private struct ProgressStep: View {
    let title: String
    @ObservedObject var model: InstallerModel
    var body: some View {
        VStack(spacing: 18) {
            Spacer()
            ProgressView().controlSize(.large)
            Text(title).font(.title3).bold()
            Text(model.progressText).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).frame(maxWidth: 420)
            Spacer()
        }
    }
}

private struct LogoutStep: View {
    @ObservedObject var model: InstallerModel
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Label("Installed", systemImage: "checkmark.circle.fill")
                .font(.title2).foregroundStyle(.green)
            Text("Almost done — one log out to go.").font(.title3).bold()
            Text("macOS registers a new input method only during login. Log out and back "
                + "in, and DS Input will activate automatically the moment you return "
                + "(we'll handle it and open Keyboard settings for you).")
                .foregroundStyle(.secondary)
            Text("Save your work first — logging out closes your apps.")
                .font(.callout).foregroundStyle(.orange)
            Spacer()
            HStack {
                Button("I'll log out later") { NSApp.terminate(nil) }
                Spacer()
                Button("Log Out Now") { model.logOutNow() }
                    .keyboardShortcut(.defaultAction)
                    .controlSize(.large)
            }
        }
    }
}

private struct FinishedStep: View {
    @ObservedObject var model: InstallerModel
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Label("DS Input is ready", systemImage: "checkmark.seal.fill")
                .font(.title2).foregroundStyle(.green)
            Text("DS Input is now active.").font(.title3).bold()
            Text(model.progressText.isEmpty
                ? "Pick DS Input from the input menu in the menu bar (the ⌨︎ icon), then "
                    + "open its Preferences and set your API key before typing."
                : model.progressText)
                .foregroundStyle(.secondary)
            Spacer()
            HStack {
                Spacer()
                Button("Done") { NSApp.terminate(nil) }
                    .keyboardShortcut(.defaultAction).controlSize(.large)
            }
        }
    }
}

private struct FailedStep: View {
    @ObservedObject var model: InstallerModel
    let message: String
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Label("Installation failed", systemImage: "xmark.octagon.fill")
                .font(.title2).foregroundStyle(.red)
            Text(message).foregroundStyle(.secondary)
            Spacer()
            HStack {
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }.controlSize(.large)
            }
        }
    }
}

// MARK: - Bits

private struct StepRow: View {
    let n: Int; let title: String; let detail: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text("\(n)").font(.headline).foregroundStyle(.white)
                .frame(width: 26, height: 26).background(Circle().fill(.tint))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(detail).font(.subheadline).foregroundStyle(.secondary)
            }
        }
    }
}

private struct StepDots: View {
    let current: InstallStep
    private var index: Int {
        switch current {
        case .welcome: return 0
        case .installing, .readyToLogout: return 1
        case .enabling, .finished: return 2
        case .failed: return 1
        }
    }
    var body: some View {
        HStack(spacing: 6) {
            ForEach(0..<3) { i in
                Circle().fill(i <= index ? Color.accentColor : Color.secondary.opacity(0.3))
                    .frame(width: 8, height: 8)
            }
        }
    }
}
