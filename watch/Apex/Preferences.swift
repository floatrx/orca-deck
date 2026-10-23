import Foundation

/// The watch's own settings, in UserDefaults. A launch argument (`-apex.layout cards`) overrides them for
/// one run without saving, which is how the UI tests pick a layout.
enum Preferences {
    /// The main page's layout: the knob, or a list of cards.
    static let layoutKey = "apex.layout"
    /// Hide worktrees with no agents at all. On by default.
    static let hideInactiveKey = "apex.hideInactive"

    static var hideInactive: Bool {
        UserDefaults.standard.object(forKey: hideInactiveKey) == nil ? true : UserDefaults.standard.bool(forKey: hideInactiveKey)
    }
}

enum Layout: String, CaseIterable, Identifiable {
    case knob, cards

    var id: String { rawValue }
    var label: String { self == .knob ? "Knob" : "Cards" }
}
