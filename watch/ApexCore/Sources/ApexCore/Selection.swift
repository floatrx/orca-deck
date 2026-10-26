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

    /// The agent calling the user: the most recent one that needs them (bucket attention, latest `since`).
    /// Apex opens on it — at launch, and on the wrist raise after a "needs you" came with the wrist down.
    public static func calling(in worktrees: [Worktree]) -> (worktreeId: String, agentId: String)? {
        var best: (worktreeId: String, agentId: String, since: Double)?
        for worktree in worktrees {
            for agent in worktree.agents where agent.bucket == .attention {
                let since = agent.since ?? 0
                if best == nil || since > best!.since { best = (worktree.id, agent.id, since) }
            }
        }
        return best.map { ($0.worktreeId, $0.agentId) }
    }

    /// The worktrees the watch shows: without the inactive ones (no agents at all) when `hideInactive`, and
    /// without the completed ones — every agent done for at least `hideCompletedAfter` seconds (nil: never) —
    /// so the knob carries what works and what calls. A fresh done stays: it is the call to send its Next.
    /// `keep` (the worktree on screen) is never hidden from under the user.
    public static func visible(_ worktrees: [Worktree], hideInactive: Bool, hideCompletedAfter: Double? = nil,
                               nowMs: Double = 0, keep: String? = nil) -> [Worktree] {
        worktrees.filter { worktree in
            if worktree.id == keep { return true }
            if hideInactive && worktree.agents.isEmpty { return false }
            if let after = hideCompletedAfter, isCompleted(worktree, for: after, nowMs: nowMs) { return false }
            return true
        }
    }

    /// Every agent is done, and the latest of them finished at least `seconds` ago.
    public static func isCompleted(_ worktree: Worktree, for seconds: Double, nowMs: Double) -> Bool {
        guard !worktree.agents.isEmpty, worktree.agents.allSatisfy({ $0.bucket == .done }) else { return false }
        let finished = worktree.agents.compactMap(\.since).max() ?? 0
        return nowMs - finished >= seconds * 1000
    }

    /// The `size` rows shown around `selected`: centred, sliding at the edges, shorter for a short list.
    public static func window(selected: Int, count: Int, size: Int) -> Range<Int> {
        guard count > 0, size > 0 else { return 0..<0 }
        let length = min(size, count)
        let start = min(max(selected - length / 2, 0), count - length)
        return start..<(start + length)
    }
}
