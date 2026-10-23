import ApexCore
import SwiftUI

/// The main page as cards (Settings → Layout → Cards): every worktree a card, the crown scrolls the list. A tap
/// focuses the worktree in Orca and makes it the agent page's worktree; a long press shows its next agent. The
/// summary takes the clock's place at the top; the chosen card glows, and so does the background, in its
/// agent's colour.
struct CardsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var keepWatching: KeepWatching
    /// This page is on screen — only then does the background animate.
    let active: Bool

    @Environment(\.isLuminanceReduced) private var dimmed
    /// A long press already acted; the button's tap on release must not focus as well.
    @State private var longPressed = false
    /// The list was scrolled to the chosen card once; after that the crown and the user own the scroll.
    @State private var centred = false

    private var bucket: Bucket? { model.shownAgent?.bucket }

    var body: some View {
        ZStack {
            TimelineView(.animation(minimumInterval: 1 / 20, paused: dimmed || !active)) { context in
                Aurora(bucket: bucket, time: context.date.timeIntervalSinceReferenceDate)
                    .id(bucket)
                    .transition(.opacity)
            }
            .animation(.easeInOut(duration: 0.6), value: bucket)
            .ignoresSafeArea()

            ScrollViewReader { proxy in
                ScrollView {
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        LazyVStack(spacing: 6) {
                            summary
                            if model.worktrees.isEmpty {
                                Text(emptyNote)
                                    .font(.system(size: 12))
                                    .foregroundStyle(Theme.muted)
                                    .padding(.top, 30)
                            }
                            ForEach(Array(model.worktrees.enumerated()), id: \.element.id) { index, worktree in
                                card(worktree, index: index, nowMs: context.date.timeIntervalSince1970 * 1000)
                                    .id(worktree.id)
                            }
                        }
                        .padding(.horizontal, 3)
                        .padding(.top, 8)
                        // The last card scrolls clear of the display's round bottom corners.
                        .padding(.bottom, 26)
                    }
                }
                .accessibilityIdentifier("cards-list")
                // The list may still be empty on appear: centre the chosen card once it arrives.
                .onAppear { centre(proxy) }
                .onChange(of: model.selectedWorktree?.id) { _, _ in centre(proxy) }
            }
            .ignoresSafeArea(.container, edges: [.top, .bottom])
        }
        .animation(.linear(duration: 0.45)) { $0.modifier(Shake(travel: CGFloat(model.pulse))) }
    }

    private func centre(_ proxy: ScrollViewProxy) {
        guard !centred, let id = model.selectedWorktree?.id else { return }
        centred = true
        proxy.scrollTo(id, anchor: .center)
    }

    /// The knob's summary line, centred in the clock's place, clear of the display's round top corners.
    private var summary: some View {
        HStack(spacing: 3) {
            if keepWatching.running {
                Image(systemName: "eye.fill").font(.system(size: 8)).foregroundStyle(Theme.muted)
            }
            Text(summaryText)
                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                .kerning(0.6)
                .textCase(.uppercase)
                .foregroundStyle(model.banner != nil ? Theme.attention : model.connection == .online ? Theme.muted : .red)
                .lineLimit(1)
                .contentTransition(.opacity)
        }
        .animation(.easeInOut(duration: 0.25), value: summaryText)
        .padding(.horizontal, 22)
        .frame(height: 16)
    }

    private var summaryText: String {
        if let banner = model.banner { return banner }
        if model.connection != .online { return model.connectionLabel }
        return Centre.summary(model.worktrees)
    }

    private var emptyNote: String {
        if model.connection != .online { return "waiting for the bridge…" }
        return model.hiddenCount > 0 ? "no active sessions" : "no workspaces"
    }

    private func card(_ worktree: Worktree, index: Int, nowMs: Double) -> some View {
        let agent = Agents.shown(worktree, picked: model.picked[worktree.id])
        let selected = index == model.selected
        return Button {
            if longPressed {
                longPressed = false
            } else {
                model.select(index)
                model.focusSelected()
            }
        } label: {
            WorktreeRow(worktree: worktree, agent: agent, selected: selected,
                        pending: model.pending == worktree.id, nowMs: nowMs)
        }
        .buttonStyle(.plain)
        .simultaneousGesture(LongPressGesture(minimumDuration: 0.45).onEnded { _ in
            longPressed = true
            model.select(index)
            model.pickNextAgent()
            // If the finger slides off, the button never fires: do not let the flag swallow a later tap.
            Task {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                longPressed = false
            }
        })
        .accessibilityIdentifier("worktree-card")
        .accessibilityValue(selected ? "selected" : "")
        .accessibilityLabel("\(worktree.name), \(Centre.state(agent))")
        .accessibilityHint("Tap to show in Orca. Swipe left for the agent.")
    }
}

/// One worktree: state dot, repo (the branch beside it) and age; state and agent count; the agent's last line.
/// The repo leads because worktree names repeat ("main" in every repo). The chosen card is outlined in its
/// agent's colour and shows more of the message.
private struct WorktreeRow: View {
    let worktree: Worktree
    let agent: Agent?
    let selected: Bool
    let pending: Bool
    let nowMs: Double

    var body: some View {
        let color = Theme.color(agent?.bucket)
        let busy = agent?.bucket == .working || agent?.bucket == .attention
        let shape = RoundedRectangle(cornerRadius: 14, style: .continuous)
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 5) {
                Circle().fill(color).frame(width: 7, height: 7)
                    .modifier(Pulse(enabled: busy))
                Text(Format.repoSubtitle(worktree) ?? worktree.name)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Theme.ink)
                    .lineLimit(1)
                    .layoutPriority(1)
                if Format.repoSubtitle(worktree) != nil {
                    Text(worktree.name)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(Theme.muted)
                        .lineLimit(1)
                }
                Spacer(minLength: 2)
                if pending {
                    ProgressView().frame(width: 12, height: 12)
                } else if let agent {
                    let age = Format.age(sinceMs: agent.since, nowMs: nowMs)
                    Text(age)
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Theme.muted)
                        .contentTransition(.numericText())
                        .animation(.snappy, value: age)
                }
            }
            Text(line)
                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                .foregroundStyle(color)
                .lineLimit(1)
                .contentTransition(.opacity)
            let detail = Centre.detail(agent)
            if !detail.isEmpty {
                Text(detail)
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.detail)
                    .lineLimit(selected ? 4 : 2)
                    .contentTransition(.opacity)
            }
        }
        .padding(.horizontal, 9)
        .padding(.vertical, 7)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(shape.fill(color.opacity(selected ? 0.2 : 0.09)))
        .overlay(shape.strokeBorder(color.opacity(selected ? 0.75 : 0), lineWidth: 1.5))
        .contentShape(shape)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: selected)
        .animation(.easeInOut(duration: 0.3), value: agent?.bucket)
        .animation(.easeInOut(duration: 0.3), value: detail)
    }

    private var detail: String { Centre.detail(agent) }

    /// "WORKING · claude · 3 agents": the state, the agent kind, the agent count.
    private var line: String {
        var parts = [Centre.state(agent)]
        if let kind = agent?.kind, !kind.isEmpty { parts.append(kind) }
        if worktree.agents.count > 1 { parts.append("\(worktree.agents.count) agents") }
        return parts.joined(separator: " · ")
    }
}
