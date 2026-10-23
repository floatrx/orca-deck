import WatchKit

/// "Keep watching": a mindfulness extended runtime session (Info.plist WKBackgroundModes = mindfulness).
/// Apple: the app keeps running after the wrist drops and may "play sounds or haptics, even after the
/// watch's screen turns off"; mindfulness stays frontmost, for at most 1 hour per session.
/// Without it, `play(_:)` does nothing once the app is inactive, and ntfy is the only wrist-down beacon.
@MainActor
final class KeepWatching: NSObject, ObservableObject, WKExtendedRuntimeSessionDelegate {
    @Published private(set) var running = false
    /// Between start() and the session's didStart — so the toggle does not flick back to off.
    @Published private(set) var starting = false
    @Published private(set) var expires: Date?
    /// Why the last session ended, when it ended early.
    @Published private(set) var note: String?

    private var session: WKExtendedRuntimeSession?

    /// Must be called while the app is active (Apple).
    func start() {
        guard session == nil else { return }
        if note != nil { note = nil }
        starting = true
        let session = WKExtendedRuntimeSession()
        session.delegate = self
        self.session = session
        session.start()
    }

    func stop() {
        session?.invalidate()
    }

    nonisolated func extendedRuntimeSessionDidStart(_ extendedRuntimeSession: WKExtendedRuntimeSession) {
        let expires = extendedRuntimeSession.expirationDate
        Task { @MainActor in
            self.running = true
            self.starting = false
            self.expires = expires
        }
    }

    nonisolated func extendedRuntimeSessionWillExpire(_ extendedRuntimeSession: WKExtendedRuntimeSession) {
        Task { @MainActor in self.note = "ends in a moment" }
    }

    nonisolated func extendedRuntimeSession(_ extendedRuntimeSession: WKExtendedRuntimeSession,
                                            didInvalidateWith reason: WKExtendedRuntimeSessionInvalidationReason,
                                            error: Error?) {
        let note = Self.describe(reason, error)
        Task { @MainActor in
            self.running = false
            self.starting = false
            self.expires = nil
            self.session = nil
            self.note = note
            WKInterfaceDevice.current().play(.stop)
        }
    }

    nonisolated private static func describe(_ reason: WKExtendedRuntimeSessionInvalidationReason, _ error: Error?) -> String? {
        switch reason {
        case .none, .sessionInProgress: return nil
        case .expired: return "the hour is up"
        case .resignedFrontmost: return "another app came to the front"
        case .suppressedBySystem: return "stopped by watchOS"
        case .error: return error?.localizedDescription ?? "session error"
        @unknown default: return "ended"
        }
    }
}
