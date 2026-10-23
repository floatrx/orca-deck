import ApexCore
import Foundation
import WatchKit

/// Polls the bridge about once a second (protocol.md: no long-poll), keeps the crown's selection on its
/// worktree, pulses for new events, focuses worktrees in Orca, and — for the agent page — fetches the
/// agent's full message and sends Next prompts with a short undo window.
@MainActor
final class AppModel: ObservableObject {
    enum Connection: Equatable {
        case connecting
        case online
        case offline(String)
        case outdated(Int)
    }

    /// The Send button's life: a countdown the user can undo, then the bridge's outcome. It belongs to the
    /// agent in `sendAgentId`; the agent page shows it only for that agent.
    enum SendState: Equatable {
        case idle
        case countdown(text: String, seconds: Int)
        /// `previous` is the bridge's last `send.at` before this send — a different one is this send's outcome.
        case sending(text: String, previous: Double?, startedAt: Date)
        case sent
        case failed(String)
    }

    /// The worktrees on the watch: the bridge's list without the inactive ones when `hideInactive` is on.
    @Published private(set) var worktrees: [Worktree] = []
    /// How many inactive worktrees `hideInactive` takes out of the list.
    @Published private(set) var hiddenCount = 0
    /// Settings: hide worktrees with no agents at all. Saved; re-filters the list at once.
    @Published var hideInactive = Preferences.hideInactive {
        didSet {
            guard hideInactive != oldValue else { return }
            UserDefaults.standard.set(hideInactive, forKey: Preferences.hideInactiveKey)
            apply(allWorktrees)
        }
    }
    @Published private(set) var connection: Connection = .connecting
    @Published private(set) var selected = 0
    /// The worktree whose `go` waits for its ack.
    @Published private(set) var pending: String?
    /// A short line about the last event or a failed command, shown for a few seconds.
    @Published private(set) var banner: String?
    /// When the last poll succeeded.
    @Published private(set) var updated: Date?
    /// Grows by one per batch of events — the knob shakes on each change.
    @Published private(set) var pulse = 0
    /// worktreeId → the agent picked with a long press; otherwise the knob shows the lead.
    @Published private(set) var picked: [String: String] = [:]
    /// The shown agent's detail while the agent page is visible.
    @Published private(set) var detail: AgentDetail?
    @Published private(set) var sendState: SendState = .idle
    /// The agent the current send belongs to — captured when Send was tapped.
    @Published private(set) var sendAgentId: String?

    /// Poll less often while the app is not frontmost-active (wrist down): saves the battery when
    /// Keep watching holds the app awake for an hour.
    /// Raising the wrist replays the beacon that came while it was down (haptics are silent then).
    var relaxed = false {
        didSet { if oldValue && !relaxed { replayMissed() } }
    }
    /// Keep watching runs: haptics play with the wrist down too, so nothing waits for the wrist.
    var buzzesInBackground = false
    /// The agent page is on screen: fetch the shown agent's detail every few seconds.
    var agentPageVisible = false {
        didSet { if agentPageVisible && !oldValue { Task { await refreshDetail() } } }
    }

    private let client: BridgeClient
    /// The strongest beacon that came while the wrist was down, and its line for the banner.
    private var missed: (beacon: Beacon, line: String)?
    /// The bridge's full list, before `hideInactive`.
    private var allWorktrees: [Worktree] = []
    private var cursor = EventCursor()
    private var selectedId: String?
    private var bannerTask: Task<Void, Never>?
    private var sendTask: Task<Void, Never>?
    private var greeted = false
    private var detailFetchedAt = Date.distantPast
    private var demoSent = false

    private static let pollInterval: UInt64 = 1_000_000_000
    private static let relaxedInterval: UInt64 = 3_000_000_000
    private static let retryInterval: UInt64 = 2_000_000_000
    private static let detailInterval: TimeInterval = 2
    /// A send with no outcome after this long is reported as failed instead of spinning forever.
    private static let sendDeadline: TimeInterval = 25
    static let undoSeconds = 3

    init(client: BridgeClient) {
        self.client = client
    }

    // MARK: - Derived

    var selectedWorktree: Worktree? {
        worktrees.indices.contains(selected) ? worktrees[selected] : nil
    }

    /// The agent the knob and the agent page show for the selected worktree.
    var shownAgent: Agent? {
        guard let worktree = selectedWorktree else { return nil }
        return Agents.shown(worktree, picked: picked[worktree.id])
    }

    /// The connection in words, for the knob and Settings.
    var connectionLabel: String {
        switch connection {
        case .connecting: return "connecting…"
        case .online: return "online"
        case .offline(let reason): return "offline · \(reason)"
        case .outdated(let version): return "update Apex · protocol v\(version)"
        }
    }

    // MARK: - Loop

    /// The poll loop; runs until the view's task is cancelled.
    func run() async {
        while !Task.isCancelled {
            let ok = await poll()
            await checkSend()
            if ok, agentPageVisible, Date().timeIntervalSince(detailFetchedAt) >= Self.detailInterval {
                await refreshDetail()
            }
            try? await Task.sleep(nanoseconds: !ok ? Self.retryInterval : relaxed ? Self.relaxedInterval : Self.pollInterval)
        }
    }

    // MARK: - Knob

    /// The crown moved to row `index` (wrapping is the view's job).
    func select(_ index: Int) {
        guard worktrees.indices.contains(index), index != selected || selectedId == nil else { return }
        selected = index
        selectedId = worktrees[index].id
        resetAgentPage()
    }

    /// Long press: show the next agent of the selected worktree.
    func pickNextAgent() {
        guard let worktree = selectedWorktree, worktree.agents.count > 1,
              let next = Agents.next(after: shownAgent?.id, in: worktree) else { return }
        picked[worktree.id] = next
        WKInterfaceDevice.current().play(.click)
        resetAgentPage()
    }

    /// A tap or Double Tap: focus the selected worktree — its shown agent's tab — in Orca on the Mac.
    func focusSelected() {
        guard let worktree = selectedWorktree, pending == nil else { return }
        pending = worktree.id
        let command = shownAgent.map { Command.go(worktree, agent: $0) } ?? Command.go(worktree)
        Task {
            defer { pending = nil }
            do {
                // No extra poll here: the bridge acks before Orca has moved, and a second poll racing the
                // loop's poll could roll the event cursor back and pulse twice.
                let ack = try await client.send(command)
                if !ack.ok {
                    Haptics.failure()
                    show("\(worktree.name): \(ack.note ?? "failed")")
                }
            } catch {
                Haptics.failure()
                show("\(worktree.name): \(error.localizedDescription)")
            }
        }
    }

    // MARK: - Agent page

    /// Send: count down `undoSeconds`, then send `text` to the shown agent — the agent captured now, even if
    /// the user turns away. Undo, or turning to another agent, cancels the countdown.
    func send(_ text: String) {
        guard let agent = shownAgent, sendTask == nil else { return }
        if case .sending = sendState { return }
        sendAgentId = agent.id
        let previous = detail?.id == agent.id ? detail?.send?.at : nil
        sendTask = Task {
            for remaining in stride(from: Self.undoSeconds, to: 0, by: -1) {
                sendState = .countdown(text: text, seconds: remaining)
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                if Task.isCancelled { return }
            }
            sendState = .sending(text: text, previous: previous, startedAt: Date())
            do {
                let ack = try await client.send(.say(agent.id, text))
                if !ack.ok { settle(ok: false, note: ack.note ?? "refused") }
            } catch {
                settle(ok: false, note: error.localizedDescription)
            }
            sendTask = nil
        }
    }

    func undoSend() {
        cancelCountdown()
        WKInterfaceDevice.current().play(.directionDown)
    }

    private func cancelCountdown() {
        sendTask?.cancel()
        sendTask = nil
        sendState = .idle
    }

    /// Settles a running send from the bridge's record for the send's own agent (not the shown one), or
    /// fails it after `sendDeadline` or when the bridge lost the record (e.g. a bridge reload).
    private func checkSend() async {
        guard case .sending(_, let previous, let startedAt) = sendState, let id = sendAgentId else { return }
        if Date().timeIntervalSince(startedAt) > Self.sendDeadline {
            settle(ok: false, note: "no answer from the bridge")
            return
        }
        guard sendTask == nil, let fetched = try? await client.agent(id: id) else { return }
        guard case .sending = sendState else { return }
        if let send = fetched.send {
            if let ok = send.ok, send.at != previous { settle(ok: ok, note: send.note) }
        } else {
            settle(ok: false, note: "the bridge lost the send — check the agent")
        }
        if fetched.id == shownAgent?.id, fetched != detail { detail = fetched }
    }

    private func settle(ok: Bool, note: String?) {
        sendState = ok ? .sent : .failed(note ?? "failed")
        if ok { WKInterfaceDevice.current().play(.success) } else { Haptics.failure() }
        if sendAgentId != shownAgent?.id,
           let worktree = worktrees.first(where: { $0.agents.contains { $0.id == sendAgentId } }) {
            show("\(worktree.name) · \(ok ? "sent" : (note ?? "send failed"))")
        }
    }

    private func refreshDetail() async {
        guard let agent = shownAgent else {
            detail = nil
            return
        }
        detailFetchedAt = Date()
        guard let fetched = try? await client.agent(id: agent.id), fetched.id == shownAgent?.id else { return }
        if fetched != detail { detail = fetched }
        if DebugLaunch.demo == "countdown", sendState == .idle, let first = fetched.next.first {
            sendAgentId = fetched.id
            sendState = .countdown(text: first, seconds: Self.undoSeconds)
        }
        // DEBUG end-to-end test: only for the exact session given by -apexPickAgent.
        if DebugLaunch.demo == "send", !demoSent, let session = DebugLaunch.pickAgent,
           fetched.id.hasPrefix("structured-agent-session-\(session):"), let first = fetched.next.first {
            demoSent = true
            send(first)
        }
    }

    private func resetAgentPage() {
        switch sendState {
        case .countdown:
            if sendAgentId != shownAgent?.id { cancelCountdown() }
        case .sent, .failed:
            sendState = .idle
        case .idle, .sending:
            break
        }
        if detail?.id != shownAgent?.id { detail = nil }
        if agentPageVisible { Task { await refreshDetail() } }
    }

    // MARK: - Polling

    @discardableResult
    private func poll() async -> Bool {
        do {
            let since = cursor.since
            let response = try await client.state(since: since)
            // A response to an older `since` is stale; applying it would roll the cursor back.
            guard cursor.since == since else { return true }
            apply(response.worktrees)
            let fresh = cursor.accept(seq: response.seq, events: response.events)
            let beacon = Beacon.pattern(for: fresh)
            // The line names the event that set the beacon: an attention beats a later done.
            let lead = fresh.last(where: { $0.kind == .attention }) ?? fresh.last
            let line = lead.map { "\($0.worktree) · \($0.kind == .attention ? "needs you" : "done")" }
            if beacon != .none, let line {
                if relaxed && !buzzesInBackground {
                    // watchOS drops haptics while the wrist is down: keep the strongest for the wrist raise.
                    if missed?.beacon != .attention { missed = (beacon, line) }
                } else {
                    Haptics.play(beacon)
                }
                pulse += 1
            }
            if let line { show(line) }
            connection = .online
            updated = Date()
            greet()
            return true
        } catch ProtocolError.unsupportedVersion(let version) {
            connection = .outdated(version)
            return false
        } catch {
            connection = .offline(Self.describe(error))
            return false
        }
    }

    private func apply(_ all: [Worktree]) {
        allWorktrees = all
        let list = Selection.visible(all, hideInactive: hideInactive)
        if list != worktrees { worktrees = list }
        if hiddenCount != all.count - list.count { hiddenCount = all.count - list.count }
        if selectedId == nil, let wanted = DebugLaunch.selectRepo,
           let index = list.firstIndex(where: { $0.repo == wanted || $0.name == wanted }) {
            selectedId = list[index].id
        }
        if let session = DebugLaunch.pickAgent, picked.isEmpty {
            for worktree in list {
                if let agent = worktree.agents.first(where: { $0.id.hasPrefix("structured-agent-session-\(session):") }) {
                    picked[worktree.id] = agent.id
                }
            }
        }
        let index = Selection.index(of: selectedId, previous: selected, in: list)
        if index != selected { selected = index }
        selectedId = list.indices.contains(index) ? list[index].id : nil
    }

    /// One line in the bridge trail (POST /api/log) — how the Mac sees what the watch measured or did.
    func report(_ text: String) {
        Task { try? await client.log(text) }
    }

    /// One line in the bridge trail per launch, so the Mac can tell the watch got through.
    private func replayMissed() {
        guard let missed else { return }
        self.missed = nil
        Haptics.play(missed.beacon)
        pulse += 1
        show("missed · " + missed.line)
    }

    private func greet() {
        guard !greeted else { return }
        greeted = true
        let device = WKInterfaceDevice.current()
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let text = "Apex \(version) online · \(device.model) watchOS \(device.systemVersion) · \(worktrees.count) worktrees"
        Task { try? await client.log(text) }
    }

    private func show(_ text: String) {
        banner = text
        bannerTask?.cancel()
        bannerTask = Task {
            try? await Task.sleep(nanoseconds: 4_000_000_000)
            if !Task.isCancelled { banner = nil }
        }
    }

    private static func describe(_ error: Error) -> String {
        if case BridgeError.status(let code) = error { return code == 401 ? "wrong token" : "HTTP \(code)" }
        if let url = error as? URLError {
            switch url.code {
            case .timedOut: return "timed out"
            case .cannotConnectToHost: return "bridge not running"
            case .notConnectedToInternet: return "no network"
            default: return "error \(url.code.rawValue)"
            }
        }
        return "bad response"
    }
}
