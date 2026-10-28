/// The prompt the agent page sends for the Next variants ticked on the watch.
public enum NextPrompt {
    /// One ticked variant goes verbatim (the bridge then drops the pending file, as Orca's → does); several go
    /// as a numbered list in the agent's order, so the recommended one stays first. nil when nothing is ticked.
    public static func compose(_ variants: [String], picked: Set<Int>) -> String? {
        let chosen = variants.indices.filter(picked.contains).map { variants[$0] }
        switch chosen.count {
        case 0: return nil
        case 1: return chosen[0]
        default: return chosen.enumerated().map { "\($0.offset + 1). \($0.element)" }.joined(separator: "\n")
        }
    }
}
