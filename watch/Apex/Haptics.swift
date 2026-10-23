import ApexCore
import WatchKit

/// How "needs you" feels. The spec asks for 2 pulses and leaves the exact haptic to feel on the wrist,
/// so Settings lets the user pick; the choice lives in UserDefaults under `key`.
enum AttentionStyle: String, CaseIterable, Identifiable {
    case clicks, notification, retry

    static let key = "attentionStyle"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .clicks: return "Two clicks"
        case .notification: return "Notification ×2"
        case .retry: return "Retry ×2"
        }
    }

    var haptic: WKHapticType {
        switch self {
        case .clicks: return .click
        case .notification: return .notification
        case .retry: return .retry
        }
    }

    static var current: AttentionStyle {
        AttentionStyle(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .clicks
    }
}

/// The beacon patterns of the spec: `done` = 1 haptic, `attention` = 2.
/// `play(_:)` has no effect while the app is inactive or in the background unless an extended runtime
/// session runs (Keep watching), and a new call waits at least 100 ms — hence the gap.
enum Haptics {
    static func play(_ beacon: Beacon, attention style: AttentionStyle = .current) {
        let device = WKInterfaceDevice.current()
        switch beacon {
        case .none:
            break
        case .done:
            device.play(.success)
        case .attention:
            device.play(style.haptic)
            DispatchQueue.main.asyncAfter(deadline: .now() + (style == .clicks ? 0.25 : 0.6)) { device.play(style.haptic) }
        }
    }

    static func failure() {
        WKInterfaceDevice.current().play(.failure)
    }
}
