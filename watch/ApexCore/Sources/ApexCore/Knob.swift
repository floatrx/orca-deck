import CoreGraphics
import Foundation

/// The knob's dial (bridge/index.html `buildDots` / `aimDots`): one dot per worktree, spread evenly
/// clockwise from 12 o'clock; the dial turns so the chosen dot sits at 12 o'clock.
public enum Dial {
    /// The dot's angle in degrees, clockwise from 12 o'clock.
    public static func angle(index: Int, count: Int) -> Double {
        count > 0 ? Double(index) * 360 / Double(count) : 0
    }

    /// The dial's rotation that brings `selected` to 12 o'clock, the short way round from `current`.
    public static func rotation(selected: Int, count: Int, current: Double) -> Double {
        let target = -angle(index: selected, count: count)
        return target + 360 * ((current - target) / 360).rounded()
    }

    /// How long a crown step's slide lasts; from then on `slide(after:)` is exactly 1.
    public static let slideSettle = 0.6

    /// A crown step's progress, 0 → 1, `seconds` after the step: a damped spring (response 0.38 s, damping
    /// 0.78 — about 2 % overshoot). The view turns it into a phase per frame, so every dot moves along the
    /// track; an implicit position animation would cut straight across the screen.
    public static func slide(after seconds: Double) -> Double {
        guard seconds > 0 else { return 0 }
        guard seconds < slideSettle else { return 1 }
        let omega = 2 * Double.pi / 0.38
        let zeta = 0.78
        let damped = omega * (1 - zeta * zeta).squareRoot()
        let envelope = exp(-zeta * omega * seconds)
        return 1 - envelope * (cos(damped * seconds) + zeta * omega / damped * sin(damped * seconds))
    }
}

/// The texts in the knob's centre, as the mock's `render()` builds them.
public enum Centre {
    /// "2 waiting · 3 working · 1 done" over every agent; "all quiet" / "no workspaces".
    public static func summary(_ worktrees: [Worktree]) -> String {
        guard !worktrees.isEmpty else { return "no workspaces" }
        let agents = worktrees.flatMap(\.agents)
        let waiting = agents.filter { $0.bucket == .attention }.count
        let working = agents.filter { $0.bucket == .working }.count
        let done = agents.filter { $0.bucket == .done }.count
        let parts = [waiting > 0 ? "\(waiting) waiting" : nil,
                     working > 0 ? "\(working) working" : nil,
                     done > 0 ? "\(done) done" : nil].compactMap { $0 }
        return parts.isEmpty ? "all quiet" : parts.joined(separator: " · ")
    }

    /// "NEEDS YOU" for attention, else Orca's state; "NO AGENTS" without an agent.
    public static func state(_ agent: Agent?) -> String {
        guard let agent else { return "NO AGENTS" }
        if agent.bucket == .attention { return "NEEDS YOU" }
        return (agent.state ?? agent.bucket.rawValue).uppercased()
    }

    /// "orca-deck · chat · claude": the repo when it differs from the name, the surface, the agent kind.
    public static func meta(_ worktree: Worktree, agent: Agent?) -> String {
        var parts: [String] = []
        if let repo = Format.repoSubtitle(worktree) { parts.append(repo) }
        if let chat = agent?.chat { parts.append(chat ? "chat" : "terminal") }
        if let kind = agent?.kind, !kind.isEmpty { parts.append(kind) }
        return parts.joined(separator: " · ")
    }

    /// "▸ tool — last" while working with a tool, else the last message or the prompt.
    public static func detail(_ agent: Agent?) -> String {
        guard let agent else { return "" }
        let last = agent.last ?? ""
        if agent.bucket == .working, let tool = agent.tool {
            return "▸ " + Format.tool(tool) + (last.isEmpty ? "" : " — " + last)
        }
        return last.isEmpty ? (agent.prompt ?? "") : last
    }

    /// "2/8 · agent 2/2 · 2m".
    public static func counter(index: Int, count: Int, worktree: Worktree, agent: Agent?, nowMs: Double) -> String {
        var text = "\(index + 1)/\(count)"
        if worktree.agents.count > 1, let agent, let position = worktree.agents.firstIndex(where: { $0.id == agent.id }) {
            text += " · agent \(position + 1)/\(worktree.agents.count)"
        }
        if let agent { text += " · " + Format.age(sinceMs: agent.since, nowMs: nowMs) }
        return text
    }
}

/// Which agent of a worktree the knob shows: the one picked with a long press, else the lead.
public enum Agents {
    public static func shown(_ worktree: Worktree, picked: String?) -> Agent? {
        if let picked, let agent = worktree.agents.first(where: { $0.id == picked }) { return agent }
        return worktree.lead ?? worktree.agents.first
    }

    /// The agent after `current`, wrapping round; nil for a worktree without agents.
    public static func next(after current: String?, in worktree: Worktree) -> String? {
        guard !worktree.agents.isEmpty else { return nil }
        let index = worktree.agents.firstIndex(where: { $0.id == current }) ?? -1
        return worktree.agents[(index + 1) % worktree.agents.count].id
    }
}

/// The knob adapted to the watch's rounded-rectangle display: dots ride a track that follows the screen's
/// shape instead of a circle. `t` is the fraction of the perimeter, 0 at top centre, growing clockwise.
/// The top and bottom corners may differ: the bottom ones follow the display's corners, the top ones sit
/// below the clock.
public enum Track {
    public static func point(at t: Double, in rect: CGRect, radius: CGFloat) -> CGPoint {
        point(at: t, in: rect, top: radius, bottom: radius)
    }

    public static func point(at t: Double, in rect: CGRect, top: CGFloat, bottom: CGFloat) -> CGPoint {
        let limit = Double(min(rect.width / 2, rect.height / 2))
        let rt = min(Double(top), limit), rb = min(Double(bottom), limit)
        let w = Double(rect.width), h = Double(rect.height)
        let x0 = Double(rect.minX), y0 = Double(rect.minY)
        let side = h - rt - rb
        let topEdge = w - 2 * rt, bottomEdge = w - 2 * rb
        let arcTop = Double.pi * rt / 2, arcBottom = Double.pi * rb / 2
        let length = topEdge + bottomEdge + 2 * side + 2 * arcTop + 2 * arcBottom
        var d = (t - t.rounded(.down)) * length

        if d <= topEdge / 2 { return CGPoint(x: x0 + w / 2 + d, y: y0) }
        d -= topEdge / 2
        if d <= arcTop { return corner(cx: x0 + w - rt, cy: y0 + rt, r: rt, from: -.pi / 2, d: d) }
        d -= arcTop
        if d <= side { return CGPoint(x: x0 + w, y: y0 + rt + d) }
        d -= side
        if d <= arcBottom { return corner(cx: x0 + w - rb, cy: y0 + h - rb, r: rb, from: 0, d: d) }
        d -= arcBottom
        if d <= bottomEdge { return CGPoint(x: x0 + w - rb - d, y: y0 + h) }
        d -= bottomEdge
        if d <= arcBottom { return corner(cx: x0 + rb, cy: y0 + h - rb, r: rb, from: .pi / 2, d: d) }
        d -= arcBottom
        if d <= side { return CGPoint(x: x0, y: y0 + h - rb - d) }
        d -= side
        if d <= arcTop { return corner(cx: x0 + rt, cy: y0 + rt, r: rt, from: .pi, d: d) }
        d -= arcTop
        return CGPoint(x: x0 + rt + d, y: y0)
    }

    /// The point `d` along a quarter arc that starts at angle `from` (radians, y grows downwards).
    private static func corner(cx: Double, cy: Double, r: Double, from: Double, d: Double) -> CGPoint {
        let angle = from + (r > 0 ? d / r : 0)
        return CGPoint(x: cx + r * cos(angle), y: cy + r * sin(angle))
    }
}

/// The crown on an unbounded range: every detent is one worktree, and the wrap is plain modulo arithmetic —
/// no reliance on how watchOS wraps a continuous range at its ends.
extension Dial {
    public static func index(forCrown value: Double, count: Int) -> Int {
        guard count > 0 else { return 0 }
        let detent = Int(value.rounded())
        return ((detent % count) + count) % count
    }

    /// The crown value nearest to `current` that selects `index`, the short way round.
    public static func crownValue(for index: Int, near current: Double, count: Int) -> Double {
        guard count > 0 else { return 0 }
        let detent = Int(current.rounded())
        var delta = index - Self.index(forCrown: current, count: count)
        if delta > count / 2 { delta -= count }
        if delta < -(count - 1) / 2 { delta += count }
        return Double(detent + delta)
    }
}
