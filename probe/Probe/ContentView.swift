import SwiftUI

/// One button per measurement of .work/notes/transport-probe.md, newest log line on top.
struct ContentView: View {
    @StateObject private var probe = Probe()

    var body: some View {
        List {
            Section("Pings ×20") {
                ForEach(probe.paths) { path in
                    if path.id == "lan" {
                        button("LAN · iPhone on") { await probe.pings(path, condition: "iPhone on") }
                        button("LAN · iPhone off") { await probe.pings(path, condition: "iPhone Wi-Fi+BT off") }
                    } else {
                        button(path.label) { await probe.pings(path, condition: "iPhone on") }
                    }
                }
            }
            Section("Wrist down 60 s") {
                button("Plain") { await probe.wristDown(extendedSession: false) }
                button("Session") { await probe.wristDown(extendedSession: true) }
            }
            Section {
                button("Haptic check") { await probe.hapticCheck() }
            }
            Section("Log") {
                ForEach(Array(probe.lines.enumerated()), id: \.offset) { _, line in
                    Text(line).font(.system(size: 12, design: .monospaced))
                }
            }
        }
        .overlay(alignment: .topTrailing) {
            if probe.busy { ProgressView().padding(4) }
        }
    }

    private func button(_ title: String, action: @escaping () async -> Void) -> some View {
        Button(title) { Task { await action() } }
            .disabled(probe.busy)
    }
}
