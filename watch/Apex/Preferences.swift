import Foundation

/// The watch's own settings, in UserDefaults. A launch argument (`-apex.layout cards`) overrides them for
/// one run without saving, which is how the UI tests pick a layout.
enum Preferences {
    /// The main page's layout: the knob, or a list of cards.
    static let layoutKey = "apex.layout"
    /// Hide worktrees with no agents at all. On by default.
    static let hideInactiveKey = "apex.hideInactive"

    /// Hide worktrees whose agents have all been done this long (seconds); 0 = never. Default: one day.
    static let hideCompletedAfterKey = "apex.hideCompletedAfter"

    static var hideCompletedAfter: Double {
        UserDefaults.standard.object(forKey: hideCompletedAfterKey) == nil ? CompletedWindow.day.rawValue
            : UserDefaults.standard.double(forKey: hideCompletedAfterKey)
    }

    static var hideInactive: Bool {
        UserDefaults.standard.object(forKey: hideInactiveKey) == nil ? true : UserDefaults.standard.bool(forKey: hideInactiveKey)
    }
}

enum Layout: String, CaseIterable, Identifiable {
    case knob, cards

    var id: String { rawValue }
    var label: String { self == .knob ? "Knob" : "Cards" }
}

/// How long a completed worktree (every agent done) stays on the main page.
enum CompletedWindow: Double, CaseIterable, Identifiable {
    case off = 0, hour = 3600, day = 86_400

    var id: Double { rawValue }
    var label: String {
        switch self {
        case .off: return "Off"
        case .hour: return "After 1 hour"
        case .day: return "After 1 day"
        }
    }
}
