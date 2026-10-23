import ApexCore
import SwiftUI

/// The page left of the main one: the main page's layout and which worktrees it lists, bridge status, Keep
/// watching, and the beacon's feel — everything the morning test needs on the wrist.
struct SettingsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var keepWatching: KeepWatching
    @AppStorage(AttentionStyle.key) private var attention = AttentionStyle.clicks.rawValue
    @AppStorage(Preferences.layoutKey) private var layout = Layout.knob

    var body: some View {
        List {
            // Centred in the clock's place (the app hides the clock), clear of the display's round corners.
            Text("Settings")
                .font(.system(size: 17, weight: .semibold))
                .frame(maxWidth: .infinity)
                .listRowBackground(Color.clear)
                .accessibilityIdentifier("settings-title")
            Section("Layout") {
                // Inline: two choices are one tap each, no drill-down.
                Picker("Layout", selection: $layout) {
                    ForEach(Layout.allCases) { layout in
                        Text(layout.label).tag(layout)
                    }
                }
                .pickerStyle(.inline)
                .labelsHidden()
                .accessibilityIdentifier("layout-picker")
            }
            Section {
                Toggle("Hide inactive", isOn: $model.hideInactive)
                    .accessibilityIdentifier("hide-inactive")
            } footer: {
                Text(model.hiddenCount > 0
                     ? "Worktrees without agents are hidden: \(model.hiddenCount) now."
                     : "Hides worktrees without agents.")
            }
            Section("Bridge") {
                LabeledContent("Status", value: model.connectionLabel)
                LabeledContent("Mac", value: Config.baseURL.host ?? Config.baseURL.absoluteString)
                if let updated = model.updated {
                    LabeledContent("Updated") {
                        Text(updated, style: .relative)
                    }
                }
            }

            Section {
                Toggle("Keep watching", isOn: Binding(
                    get: { keepWatching.running || keepWatching.starting },
                    set: { $0 ? keepWatching.start() : keepWatching.stop() }
                ))
                if let expires = keepWatching.expires {
                    LabeledContent("Until") {
                        Text(expires, style: .time)
                    }
                }
                if let note = keepWatching.note {
                    Text(note).font(.footnote).foregroundStyle(.secondary)
                }
            } footer: {
                Text("Keeps Apex awake with your wrist down for up to an hour, so it can still buzz. Uses more battery.")
            }

            Section("Haptics") {
                Picker("Needs you", selection: $attention) {
                    ForEach(AttentionStyle.allCases) { style in
                        Text(style.label).tag(style.rawValue)
                    }
                }
                Button("Try “done”") { Haptics.play(.done) }
                Button("Try “needs you”") { Haptics.play(.attention) }
            }

            Section {
                Text("Apex \(version) · protocol v\(ProtocolVersion.supported)")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                if let expires = Signing.expiration {
                    LabeledContent("Signed until") {
                        Text(expires, format: .dateTime.day().month(.abbreviated).hour().minute())
                    }
                    .font(.footnote)
                }
            } footer: {
                if Signing.expiration != nil {
                    Text("A free developer account signs for 7 days: reinstall with watch/install.sh before then.")
                }
            }
        }
        .ignoresSafeArea(.container, edges: .top)
    }

    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
    }
}
