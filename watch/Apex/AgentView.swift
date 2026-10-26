import ApexCore
import SwiftUI

/// Page 1 — the chosen worktree's agent: a header pinned where the system clock would be (the app hides it),
/// its full last message (scrolls with the crown), and the Next card pinned at the bottom — the page's goal,
/// so Send never needs a scroll. Send waits a short undo window before the prompt goes to the agent. The
/// background glows in the agent's colour, like the knob.
struct AgentView: View {
    @ObservedObject var model: AppModel
    /// This page is on screen — only then does its scroll view own the crown and the glow animate.
    let active: Bool

    @Environment(\.isLuminanceReduced) private var dimmed
    @State private var chosen = 0
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
                        ScrollView {
                            VStack(alignment: .leading, spacing: 8) {
                                messageCard(worktree, agent)
                                showInOrca
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
                .onChange(of: agent.id) { _, _ in chosen = 0 }
            } else {
                Text("No agent here")
                    .font(.footnote)
                    .foregroundStyle(Theme.muted)
            }
        }
        .onChange(of: active) { _, isActive in focused = isActive }
        .onAppear { focused = active }
    }

    /// The Next variants — none while a question is pending: they belong to the previous turn.
    private var variants: [String] {
        guard let detail = model.detail, detail.id == model.shownAgent?.id, detail.question == nil else { return [] }
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

    /// The question and one button per option; a tap counts down an answer (Undo, like Send). While the answer
    /// counts down or runs, the chosen option replaces the list and the control is pinned at the bottom.
    private func questionPage(_ question: AgentQuestion, _ agent: Agent) -> some View {
        let state = sendState(for: agent)
        let chosen: String? = {
            switch state {
            case .countdown(let text, _), .sending(let text, _, _): return text
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
                pinnedCard { sendControl(text: chosen ?? "", state: state) }
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
            Text(worktree.name)
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
                HStack(spacing: 4) {
                    ProgressView().frame(width: 10, height: 10)
                    Text("loading the full message")
                        .font(.system(size: 9))
                        .foregroundStyle(Theme.muted)
                }
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

    /// The chosen Next variant (a tap cycles through the variants) and the Send control. While a send counts
    /// down or runs, the card shows the text that is being sent and the variants cannot change.
    private func nextCard(_ agent: Agent) -> some View {
        let state = sendState(for: agent)
        let locked: String? = {
            switch state {
            case .countdown(let text, _), .sending(let text, _, _): return text
            default: return nil
            }
        }()
        let index = min(chosen, max(variants.count - 1, 0))
        let text = locked ?? (variants.isEmpty ? nil : variants[index])
        return VStack(alignment: .leading, spacing: 5) {
            if let text {
                Button {
                    chosen = (index + 1) % max(variants.count, 1)
                } label: {
                    HStack(alignment: .top, spacing: 5) {
                        Image(systemName: "arrow.right.circle.fill")
                            .font(.system(size: 12))
                            .foregroundStyle(Theme.attention)
                        Text(text)
                            .font(.system(size: 12))
                            .foregroundStyle(Theme.ink)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentTransition(.opacity)
                        if variants.count > 1 && locked == nil {
                            Text("\(index + 1)/\(variants.count)")
                                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                                .foregroundStyle(Theme.muted)
                                .contentTransition(.numericText())
                        }
                    }
                }
                .buttonStyle(.plain)
                .disabled(locked != nil)
                .animation(.snappy, value: chosen)
                .accessibilityHint(variants.count > 1 && locked == nil ? "Tap for the next variant" : "")
                sendControl(text: text, state: state)
            } else {
                outcome(state)
            }
        }
        .modifier(PinnedCardFrame(bottomRadius: $cardBottomRadius))
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: state)
    }

    /// A card pinned at the bottom of the page, its bottom corners concentric with the display's.
    private func pinnedCard<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 5) { content() }
            .modifier(PinnedCardFrame(bottomRadius: $cardBottomRadius))
    }

    /// The card without variants: the outcome of the last send (a sent variant leaves the list), else "No Next".
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
        case .idle, .countdown, .sending:
            Label("No Next", systemImage: "arrow.right.circle")
                .font(.system(size: 12))
                .foregroundStyle(Theme.muted)
        }
    }

    @ViewBuilder
    private func sendControl(text: String, state: AppModel.SendState) -> some View {
        switch state {
        case .countdown(_, let seconds):
            Button {
                model.undoSend()
            } label: {
                Text("\(model.delivery == .answer ? "Answering" : "Sending") in \(seconds) · Undo")
                    .contentTransition(.numericText(countsDown: true))
            }
            .accessibilityIdentifier("undo-button")
            .buttonStyle(Capsule40(fill: Theme.attention.opacity(0.22), foreground: Theme.attention,
                                   progress: Double(seconds - 1) / Double(AppModel.undoSeconds)))
            .animation(.linear(duration: 1), value: seconds)
        case .sending:
            HStack(spacing: 6) {
                ProgressView().frame(width: 14, height: 14)
                Text(model.delivery == .answer ? "Answering…" : "Sending…").font(.system(size: 12)).foregroundStyle(Theme.muted)
            }
            .frame(maxWidth: .infinity, minHeight: 40)
        case .idle, .sent, .failed:
            VStack(spacing: 4) {
                if state == .sent {
                    Label(model.delivery == .answer ? "Answered" : "Sent", systemImage: "checkmark.circle.fill")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.done)
                        .transition(.scale(scale: 0.6).combined(with: .opacity))
                }
                if case .failed(let note) = state {
                    Text(note).font(.system(size: 10)).foregroundStyle(.red).lineLimit(2)
                }
                Button {
                    model.send(text)
                } label: {
                    Label("Send", systemImage: "paperplane.fill")
                }
                .buttonStyle(Capsule40(fill: Theme.attention, foreground: .white))
                .accessibilityIdentifier("send-button")
            }
        }
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

/// A full-width 40 pt capsule: watchOS's default button is ~60 pt tall, which leaves the message one line.
/// `progress` (0…1) draws a fill that drains — the undo countdown.
private struct Capsule40: ButtonStyle {
    let fill: Color
    let foreground: Color
    var progress: Double? = nil

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(foreground)
            .frame(maxWidth: .infinity, minHeight: 40)
            .background(
                ZStack(alignment: .leading) {
                    Capsule().fill(fill)
                    if let progress {
                        GeometryReader { geometry in
                            Capsule()
                                .fill(foreground.opacity(0.28))
                                .frame(width: geometry.size.width * max(0, min(1, progress)))
                        }
                    }
                }
            )
            .clipShape(Capsule())
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
            .contentShape(Capsule())
    }
}
