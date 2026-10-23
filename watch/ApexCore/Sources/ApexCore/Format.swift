/// Display helpers, matching the browser mock (bridge/index.html).
public enum Format {
    /// "4s", "2m", "3h", "2d"; "" without a timestamp. Never negative.
    public static func age(sinceMs: Double?, nowMs: Double) -> String {
        guard let sinceMs else { return "" }
        let seconds = max(0, ((nowMs - sinceMs) / 1000).rounded())
        if seconds < 60 { return "\(Int(seconds))s" }
        if seconds < 3600 { return "\(Int((seconds / 60).rounded()))m" }
        if seconds < 86400 { return "\(Int((seconds / 3600).rounded()))h" }
        return "\(Int((seconds / 86400).rounded()))d"
    }

    /// `mcp__plugin_playwright_playwright__browser_take_screenshot` → `browser_take_screenshot`.
    public static func tool(_ name: String) -> String {
        name.components(separatedBy: "__").last ?? name
    }

    /// The repo under the worktree name, only when it adds information.
    public static func repoSubtitle(_ worktree: Worktree) -> String? {
        guard let repo = worktree.repo, !repo.isEmpty, repo != worktree.name else { return nil }
        return repo
    }
}
