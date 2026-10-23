// THROWAWAY transport probe for orca-deck (Phase 2 of .work/plans/watch-remote.md).
// Measures which network path a real Apple Watch can use to reach the bridge, and what survives
// a lowered wrist. The simulator allows networking the device blocks (TN3135) — run on the watch.
import SwiftUI

@main
struct ProbeApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
