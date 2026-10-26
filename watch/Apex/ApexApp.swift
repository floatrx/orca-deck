// Apex — the orca-deck Apple Watch remote. The crown picks a worktree (one haptic tick per row),
// a tap or Double Tap focuses it in Orca on the Mac, and the wrist buzzes when an agent finishes or needs you.
import SwiftUI

@main
struct ApexApp: App {
    @StateObject private var model = AppModel(client: BridgeClient(routes: Config.routes, token: Config.token))
    @StateObject private var keepWatching = KeepWatching()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            NavigationStack {
                ContentView(model: model, keepWatching: keepWatching)
            }
            // On the container, not the list: pushing Settings must not cancel the poll loop.
            .task { await model.run() }
            .onChange(of: scenePhase) { _, phase in model.relaxed = phase != .active }
            .onChange(of: keepWatching.running) { _, running in model.buzzesInBackground = running }
        }
    }
}
