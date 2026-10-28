import ApexCore
import SwiftUI

/// Page 1 — the chosen worktree's agent: a header pinned where the system clock would be (the app hides it),
/// its full last message (scrolls with the crown), and the Next card pinned at the bottom — the page's goal,
/// so Send never needs a scroll; a tick per Next variant under the message, the recommended one ticked. A send
/// goes to the agent at once. The background glows in the agent's
/// colour, like the knob.
struct AgentView: View {
    @ObservedObject var model: AppModel
    /// This page is on screen — only then does its scroll view own the crown and the glow animate.
    let active: Bool

    @Environment(\.isLuminanceReduced) private var dimmed
    /// The ticked Next variants (indices); the recommended first one is ticked whenever the variants change.
    @State private var picked: Set<Int> = DebugLaunch.pickVariants ?? [0]
    /// The Next card's bottom corners, concentric with the display's (ConcentricRadius).
    @State private var cardBottomRadius: CGFloat = 32
    @FocusState private var focused: Bool

    var body: some View {
        ZStack {
            TimelineView(.animation(minimumInterval: 1 / 20, paused: dimmed || !active)) { context in
                Aurora(bucket: model.shownAgent?.bucket, time: context.date.timeIntervalSinceReferenceDate)
                    .id(model.shownAgent?.bucket)
                    .transition(.opacity)
            }
            .animation(.easeInOut(duration: 0.6), value: model.shownAgent?.bucket)
            .ignoresSafeArea()

            if let worktree = model.selectedWorktree, let agent = model.shownAgent {
                VStack(spacing: 5) {
                    header(worktree, agent)
                    if let question = pendingQuestion(of: agent) {
                        // The agent waits on a question: answering it is the page's goal, and the Next variants
                        // belong to the previous turn — no Next card, no Send.
                        questionPage(question, agent)
                    } else {
                        // The ticks follow the message inside the scroll, like a question's options: three
                        // two-line variants pinned at the bottom left the message one line. The page opens
                        // scrolled to them; the crown goes up to read the message.
                        ScrollViewReader { proxy in
                            ScrollView {
                                VStack(alignment: .leading, spacing: 8) {
                                    messageCard(worktree, agent)
                                    if showsTicks(agent) {
                                        variantList.id(Self.ticksAnchor)
                                    }
                                    showInOrca
                                }
                            }
                            // Opened scrolled, the message would draw over the header without a clip.
                            .clipped()
                            .onAppear { proxy.scrollTo(Self.ticksAnchor, anchor: .bottom) }
                            .onChange(of: variants) { _, _ in
                                withAnimation(.easeInOut(duration: 0.35)) {
                                    proxy.scrollTo(Self.ticksAnchor, anchor: .bottom)
                                }
                            }
                        }
                        .focused($focused)
                        .accessibilityIdentifier("agent-page")
                        nextCard(agent)
                    }
                }
                .padding(.horizontal, 2)
                // Edge to edge: the header takes the clock's place at the top, and the Next card hugs the
                // bottom, its concentric corners inside the display's curve.
                .padding(.top, 8)
                .padding(.bottom, 6)
                .ignoresSafeArea(.container, edges: [.top, .bottom])
                .onChange(of: agent.id) { _, _ in picked = [0] }
                .onChange(of: variants) { _, _ in picked = [0] }
            } else {
                Text("No agent here")
                    .font(.footnote)
                    .foregroundStyle(Theme.muted)
            }
        }
        .onChange(of: active) { _, isActive in focused = isActive }
        .onAppear { focused = active }
    }

    private static let ticksAnchor = "next-ticks"

    /// The ticks are shown while there are variants and no send of this agent runs.
    private func showsTicks(_ agent: Agent) -> Bool {
        if case .sending = sendState(for: agent) { return false }
        return !variants.isEmpty
    }

    /// The Next variants — none while a question is pending or the agent works: they belong to the previous
    /// turn, and the Stop hook writes fresh ones when this turn ends (user, 2026-10-28).
    private var variants: [String] {
        if DebugLaunch.demo == "variants" { return DebugLaunch.demoVariants }
        guard let detail = model.detail, detail.id == model.shownAgent?.id, detail.question == nil,
              model.shownAgent?.bucket != .working else { return [] }
        return detail.next
    }

    /// The question the shown agent waits on, from its detail.
    private func pendingQuestion(of agent: Agent) -> AgentQuestion? {
        guard let detail = model.detail, detail.id == agent.id else { return nil }
        return detail.question
    }

    private var showInOrca: some View {
        Button {
            model.focusSelected()
        } label: {
            Label("Show in Orca", systemImage: "macwindow")
        }
        .buttonStyle(Capsule40(fill: Color.white.opacity(0.14), foreground: Theme.ink))
    }

    /// The question and one button per option; a tap answers at once. While the answer runs, the chosen option
    /// replaces the list and the progress is pinned at the bottom.
    private func questionPage(_ question: AgentQuestion, _ agent: Agent) -> some View {
        let state = sendState(for: agent)
        let chosen: String? = {
            switch state {
            case .sending(let text, _, _): return text
            default: return nil
            }
        }()
        return VStack(spacing: 5) {
            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    questionCard(question)
                    if let chosen {
                        Label(chosen, systemImage: "checkmark.circle.fill")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(Theme.attention)
                            .lineLimit(2)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 8)
                            .transition(.opacity)
                    } else if question.answerable {
                        ForEach(question.options) { option in
                            Button {
                                model.answer(option, to: question)
                            } label: {
                                Text(option.label)
                            }
                            .buttonStyle(OptionStyle())
                            .accessibilityIdentifier("answer-option")
                        }
                    } else {
                        // Not one tap (multi-select, several questions, free text): the choices, read-only.
                        ForEach(question.options) { option in
                            Text("· " + option.label)
                                .font(.system(size: 12))
                                .foregroundStyle(Theme.detail)
                                .lineLimit(2)
                                .padding(.horizontal, 8)
                        }
                        Label("Answer in Orca", systemImage: "macwindow")
                            .font(.system(size: 12))
                            .foregroundStyle(Theme.muted)
                            .padding(.horizontal, 8)
                    }
                    if case .failed(let note) = state {
                        Text(note).font(.system(size: 11)).foregroundStyle(.red).lineLimit(3).padding(.horizontal, 8)
                    }
                    showInOrca
                }
                .animation(.easeInOut(duration: 0.25), value: chosen)
            }
            .focused($focused)
            .accessibilityIdentifier("agent-page")
            if chosen != nil {
                pinnedCard { sendingRow }
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: chosen)
    }

    private func questionCard(_ question: AgentQuestion) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Label(question.header ?? "Question", systemImage: "questionmark.bubble.fill")
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .textCase(.uppercase)
                .foregroundStyle(Theme.attention)
                .lineLimit(1)
            Text(question.text)
                .font(.system(size: 13))
                .foregroundStyle(Theme.ink)
                .accessibilityIdentifier("agent-question")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 12).fill(Theme.attention.opacity(0.14)))
    }

    /// One line pinned at the top, in the system clock's place: which agent this is, its state and age. The
    /// display's top corners are round, so it keeps clear of them.
    private func header(_ worktree: Worktree, _ agent: Agent) -> some View {
        HStack(spacing: 5) {
            Circle().fill(Theme.color(agent.bucket)).frame(width: 6, height: 6)
                .modifier(Pulse(enabled: agent.bucket == .working || agent.bucket == .attention))
            Text(Centre.title(worktree))
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Theme.ink)
                .lineLimit(1)
            Text(Centre.state(agent))
                .font(.system(size: 8, weight: .bold, design: .monospaced))
                .kerning(0.8)
                .foregroundStyle(Theme.color(agent.bucket))
                .lineLimit(1)
                .contentTransition(.opacity)
            Spacer(minLength: 2)
            TimelineView(.periodic(from: .now, by: 1)) { context in
                let age = Format.age(sinceMs: agent.since, nowMs: context.date.timeIntervalSince1970 * 1000)
                Text(age)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Theme.muted)
                    .contentTransition(.numericText())
                    .animation(.snappy, value: age)
            }
        }
        .animation(.easeInOut(duration: 0.3), value: agent.bucket)
        .padding(.horizontal, 22)
        .frame(height: 16)
    }

    private func messageCard(_ worktree: Worktree, _ agent: Agent) -> some View {
        let detail = model.detail?.id == agent.id ? model.detail : nil
        // An empty full message (a turn of tool calls only) falls back to Orca's 200-character cut.
        let full = detail?.message ?? ""
        let message = full.isEmpty ? (agent.last ?? "") : full
        return VStack(alignment: .leading, spacing: 4) {
            if message.isEmpty {
                Text(agent.bucket == .working ? "Working…" : "No message yet")
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.muted)
            } else {
                // Agents answer in Markdown: render the inline part (bold, italics, `code`), keep line breaks.
                Text((try? AttributedString(markdown: message,
                                            options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
                     ?? AttributedString(message))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.ink)
                    .transition(.opacity)
                    .id(message)
                    .accessibilityIdentifier("agent-message")
            }
            if detail == nil || detail?.full == false {
                // The system ProgressView ignores its frame on watchOS and dwarfs small text: a text-sized one.
                Label {
                    Text("loading the full message")
                } icon: {
                    Spinner()
                }
                .font(.system(size: 10))
                .foregroundStyle(Theme.muted)
                .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.35), value: message)
        .animation(.easeInOut(duration: 0.35), value: detail?.full)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(0.09)))
    }

    /// The send state of this agent's send; another agent's send in flight shows as idle here.
    private func sendState(for agent: Agent) -> AppModel.SendState {
        model.sendAgentId == agent.id ? model.sendState : .idle
    }

    /// Send for the ticked Next variants (the ticks sit under the message) — often more than one is wanted. The
    /// first variant is the agent's recommendation: ticked from the start, so one tap on Send still sends it.
    /// While a send runs, the card shows the text that is being sent.
    private func nextCard(_ agent: Agent) -> some View {
        let state = sendState(for: agent)
        let locked: String? = {
            switch state {
            case .sending(let text, _, _): return text
            default: return nil
            }
        }()
        return VStack(alignment: .leading, spacing: 5) {
            if let locked {
                Label(locked, systemImage: "arrow.right.circle.fill")
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.ink)
                    .lineLimit(3)
                    .frame(maxWidth: .infinity, alignment: .leading)
                sendingRow
            } else if variants.isEmpty {
                outcome(state)
            } else {
                if state == .sent {
                    Label(model.delivery == .answer ? "Answered" : "Sent", systemImage: "checkmark.circle.fill")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.done)
                        .transition(.scale(scale: 0.6).combined(with: .opacity))
                }
                if case .failed(let note) = state {
                    Text(note).font(.system(size: 10)).foregroundStyle(.red).lineLimit(2)
                }
                sendPicked
            }
        }
        .modifier(PinnedCardFrame(bottomRadius: $cardBottomRadius))
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: state)
    }

    /// The variant rows under the message; a tap ticks or unticks one.
    private var variantList: some View {
        VStack(spacing: 4) {
            ForEach(Array(variants.enumerated()), id: \.offset) { index, variant in
                let ticked = picked.contains(index)
                Button {
                    if ticked { picked.remove(index) } else { picked.insert(index) }
                } label: {
                    HStack(alignment: .center, spacing: 6) {
                        Image(systemName: ticked ? "checkmark.circle.fill" : "circle")
                            .font(.system(size: 14))
                            .foregroundStyle(ticked ? Theme.attention : Theme.muted)
                            .contentTransition(.symbolEffect(.replace))
                        Text(variant)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        if index == 0 {
                            Image(systemName: "star.fill")
                                .font(.system(size: 9))
                                .foregroundStyle(Theme.attention)
                                .accessibilityLabel("Recommended")
                        }
                    }
                }
                .buttonStyle(VariantStyle(ticked: ticked, recommended: index == 0))
                .accessibilityIdentifier("next-variant")
                .accessibilityValue(ticked ? "ticked" : "")
            }
        }
        .animation(.snappy(duration: 0.2), value: picked)
    }

    /// Send for the ticked variants: one goes verbatim, several as a numbered list (NextPrompt).
    private var sendPicked: some View {
        let text = NextPrompt.compose(variants, picked: picked)
        return Button {
            if let text { model.send(text) }
        } label: {
            if text == nil {
                Text("Tick a variant")
            } else {
                Label(picked.count > 1 ? "Send \(picked.count)" : "Send", systemImage: "paperplane.fill")
                    .contentTransition(.numericText())
            }
        }
        .disabled(text == nil)
        .buttonStyle(Capsule40(fill: text == nil ? Color.white.opacity(0.10) : Theme.attention,
                               foreground: text == nil ? Theme.muted : .white))
        .animation(.snappy, value: picked.count)
        .accessibilityIdentifier("send-button")
    }

    /// A card pinned at the bottom of the page, its bottom corners concentric with the display's.
    private func pinnedCard<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 5) { content() }
            .modifier(PinnedCardFrame(bottomRadius: $cardBottomRadius))
    }

    /// The card without variants: the outcome of the last send (a sent variant leaves the list), else "Working…"
    /// while the agent works (its Next comes when the turn ends), else "No Next".
    @ViewBuilder
    private func outcome(_ state: AppModel.SendState) -> some View {
        switch state {
        case .sent:
            Label(model.delivery == .answer ? "Answered" : "Sent", systemImage: "checkmark.circle.fill")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(Theme.done)
                .transition(.scale(scale: 0.6).combined(with: .opacity))
        case .failed(let note):
            Text(note).font(.system(size: 11)).foregroundStyle(.red).lineLimit(2)
        case .idle, .sending:
            if model.shownAgent?.bucket == .working {
                Label {
                    Text("Working…")
                } icon: {
                    Spinner()
                }
                .font(.system(size: 12))
                .foregroundStyle(Theme.color(.working))
                .accessibilityIdentifier("next-working")
            } else {
                Label("No Next", systemImage: "arrow.right.circle")
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.muted)
            }
        }
    }

    /// A delivery on its way: the bridge's outcome settles it.
    private var sendingRow: some View {
        Label {
            Text(model.delivery == .answer ? "Answering…" : "Sending…")
        } icon: {
            Spinner()
        }
        .font(.system(size: 12))
        .foregroundStyle(Theme.muted)
        .frame(maxWidth: .infinity, minHeight: 40)
    }
}

/// A spinner the size of the text next to it: it takes the surrounding font.
private struct Spinner: View {
    var body: some View {
        Image(systemName: "progress.indicator")
            .symbolEffect(.variableColor.iterative, options: .repeating)
            .accessibilityLabel("Loading")
    }
}

/// The Next card's frame, shared by every card pinned at the bottom: padding, a tint, and bottom corners
/// concentric with the display (ConcentricRadius).
private struct PinnedCardFrame: ViewModifier {
    @Binding var bottomRadius: CGFloat

    func body(content: Content) -> some View {
        content
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(8)
            .padding(.bottom, max(0, bottomRadius - 26) / 2)
            .background(
                UnevenRoundedRectangle(topLeadingRadius: 12, bottomLeadingRadius: bottomRadius,
                                       bottomTrailingRadius: bottomRadius, topTrailingRadius: 12, style: .circular)
                    .fill(Theme.attention.opacity(0.12))
            )
            .modifier(ConcentricRadius(edge: .bottom, fallback: 32, radius: $bottomRadius))
    }
}

/// An answer option: full width, at least 40 pt, two lines of label — a rounded card rather than a capsule,
/// because option labels wrap.
private struct OptionStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(Theme.ink)
            .multilineTextAlignment(.leading)
            .lineLimit(2)
            .frame(maxWidth: .infinity, minHeight: 40, alignment: .leading)
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.attention.opacity(0.24)))
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
            .contentShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// A Next variant row: full width, two lines of label. A ticked row is tinted in the attention colour, an unticked
/// one is quiet; the recommended row's label is semibold.
private struct VariantStyle: ButtonStyle {
    let ticked: Bool
    let recommended: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: recommended ? .semibold : .regular))
            .foregroundStyle(ticked ? Theme.ink : Theme.detail)
            .multilineTextAlignment(.leading)
            .lineLimit(2)
            .frame(maxWidth: .infinity, minHeight: 32, alignment: .leading)
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(ticked ? Theme.attention.opacity(0.26) : Color.white.opacity(0.08)))
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
            .contentShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}

/// A full-width 40 pt capsule: watchOS's default button is ~60 pt tall, which leaves the message one line.
private struct Capsule40: ButtonStyle {
    let fill: Color
    let foreground: Color

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(foreground)
            .frame(maxWidth: .infinity, minHeight: 40)
            .background(Capsule().fill(fill))
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
            .contentShape(Capsule())
    }
}
