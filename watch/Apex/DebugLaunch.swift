import Foundation

/// Launch arguments for simulator screenshots (DEBUG builds only), because `simctl` cannot swipe or tap:
///   xcrun simctl launch <sim> com.floatrx.orcadeck.apex -apexPage 1 -apexSelectRepo orca-deck -apexDemo countdown
/// - `-apexPage 1` opens the agent page.
/// - `-apexSelectRepo <repo or name>` starts on that worktree instead of Orca's active one.
/// - `-apexDemo countdown` shows the Send button's undo countdown without sending anything.
/// - `-apexDemo variants` shows three sample Next variants on the agent page (nothing is sent unless tapped).
/// - `-apexPickVariants 0,2` ticks those Next variants instead of the first one.
/// - `-apexPickAgent <session id>` shows that agent of its worktree (the long-press choice).
/// - `-apexCornerOffset <pt>` adds points to the knob track's corner radii — renders corner variants to compare.
/// - `-apexDemo send` presses Send on the first Next variant — only when the shown agent's id is exactly that
///   session (`structured-agent-session-<session id>:…`): an end-to-end test for the scratch session only.
enum DebugLaunch {
    static let demoVariants = ["commit and push", "run the e2e suite before committing",
                               "switch to the sandbox setup from todo.md"]

    #if DEBUG
    private static func value(_ name: String) -> String? {
        let arguments = ProcessInfo.processInfo.arguments
        guard let index = arguments.firstIndex(of: "-" + name), index + 1 < arguments.count else { return nil }
        return arguments[index + 1]
    }

    static var page: Int { Int(value("apexPage") ?? "") ?? 0 }
    static var selectRepo: String? { value("apexSelectRepo") }
    static var demo: String? { value("apexDemo") }
    static var pickAgent: String? { value("apexPickAgent") }
    static var cornerOffset: CGFloat { CGFloat(Double(value("apexCornerOffset") ?? "") ?? 0) }
    static var pickVariants: Set<Int>? {
        value("apexPickVariants").map { Set($0.split(separator: ",").compactMap { Int($0) }) }
    }
    #else
    static let page = 0
    static let selectRepo: String? = nil
    static let demo: String? = nil
    static let pickAgent: String? = nil
    static let cornerOffset: CGFloat = 0
    static let pickVariants: Set<Int>? = nil
    #endif
}
