/// The crown's choice in a worktree list that changes under it.
public enum Selection {
    /// The row for `selectedId` in `worktrees`. Without a selection yet: the active worktree, else the top.
    /// A selection that vanished keeps its position, clamped to the list.
    public static func index(of selectedId: String?, previous: Int, in worktrees: [Worktree]) -> Int {
        guard !worktrees.isEmpty else { return 0 }
        guard let selectedId else { return worktrees.firstIndex(where: \.active) ?? 0 }
        if let index = worktrees.firstIndex(where: { $0.id == selectedId }) { return index }
        return min(max(previous, 0), worktrees.count - 1)
    }

    /// The worktrees the watch shows: without the inactive ones (no agents at all) when `hideInactive`.
    public static func visible(_ worktrees: [Worktree], hideInactive: Bool) -> [Worktree] {
        hideInactive ? worktrees.filter { !$0.agents.isEmpty } : worktrees
    }

    /// The `size` rows shown around `selected`: centred, sliding at the edges, shorter for a short list.
    public static func window(selected: Int, count: Int, size: Int) -> Range<Int> {
        guard count > 0, size > 0 else { return 0..<0 }
        let length = min(size, count)
        let start = min(max(selected - length / 2, 0), count - length)
        return start..<(start + length)
    }
}
