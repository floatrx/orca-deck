import Foundation
import WatchKit

/// One network path to the bridge's remote API (port 7353).
struct ProbePath: Identifiable, Hashable {
    let id: String
    let label: String
    let base: URL
}

/// The measurements of .work/notes/transport-probe.md. Every summary goes to the screen and,
/// through `POST /api/log`, over every path to the bridge trail — whatever arrives, arrives.
@MainActor
final class Probe: ObservableObject {
    @Published private(set) var lines: [String] = []
    @Published private(set) var busy = false

    let paths: [ProbePath]
    private let session: URLSession
    private let extended = ExtendedSession()

    init() {
        var paths: [ProbePath] = []
        if let lan = URL(string: Secrets.lanBase) { paths.append(ProbePath(id: "lan", label: "LAN", base: lan)) }
        if !Secrets.tunnelBase.isEmpty, let tunnel = URL(string: Secrets.tunnelBase) {
            paths.append(ProbePath(id: "tunnel", label: "Tunnel", base: tunnel))
        }
        self.paths = paths

        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 5
        config.timeoutIntervalForResource = 8
        config.waitsForConnectivity = false
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: config)

        extended.onLog = { [weak self] text in
            Task { @MainActor in self?.note(text) }
        }
    }

    // MARK: - Measurements 1 and 2: 20 sequential pings on one path

    func pings(_ path: ProbePath, condition: String, count: Int = 20) async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        note("▶ \(path.label) ×\(count) · \(condition)")
        var times: [Double] = []
        var errors: [String: Int] = [:]
        for _ in 0..<count {
            switch await ping(path) {
            case .success(let ms): times.append(ms)
            case .failure(let error): errors[describe(error), default: 0] += 1
            }
        }
        let summary = "\(path.label) · \(condition) · ok \(times.count)/\(count) · p50 \(percentile(times, 0.5)) · p95 \(percentile(times, 0.95))"
            + (errors.isEmpty ? "" : " · errors " + errors.map { "\($0.key)×\($0.value)" }.joined(separator: ", "))
        note(summary)
        await report("probe \(summary)")
    }

    // MARK: - Measurements 4 and 5: 60 s with the wrist down

    /// Polls /api/ping every 2 s on the first path that answers, plays a click every 10 s,
    /// and a success at 60 s. Each tick records the app state, so suspension shows up as gaps.
    func wristDown(extendedSession: Bool) async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        let mode = extendedSession ? "session" : "plain"
        guard let path = await firstReachable() else {
            note("✗ wrist-down \(mode): no path answers")
            return
        }
        if extendedSession { extended.start() }
        note("▶ wrist-down \(mode) via \(path.label): lower your wrist now, count the clicks (expect 6)")
        WKInterfaceDevice.current().play(.start)

        let start = Date()
        var ticks: [String] = []
        var attempted = 0, ok = 0, inactiveAttempted = 0, inactiveOk = 0, haptics = 0
        var lastTick = start
        var maxGap = 0.0
        var nextHaptic = 10.0
        while Date().timeIntervalSince(start) < 60 {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            let now = Date()
            let t = now.timeIntervalSince(start)
            maxGap = max(maxGap, now.timeIntervalSince(lastTick))
            lastTick = now
            let state = WKApplication.shared().applicationState
            let inactive = state != .active
            attempted += 1
            if inactive { inactiveAttempted += 1 }
            let result = await ping(path)
            if case .success = result {
                ok += 1
                if inactive { inactiveOk += 1 }
            }
            if t >= nextHaptic {
                WKInterfaceDevice.current().play(.click)
                haptics += 1
                nextHaptic += 10
            }
            ticks.append(String(format: "%.0f:%@:%@", t, stateName(state), resultMark(result)))
        }
        WKInterfaceDevice.current().play(.success)
        if extendedSession { extended.stop() }
        let summary = "wrist-down \(mode) via \(path.label) · polls ok \(ok)/\(attempted) · inactive ok \(inactiveOk)/\(inactiveAttempted)"
            + String(format: " · max gap %.1f s", maxGap) + " · haptics called \(haptics)"
        note(summary)
        await report("probe \(summary) · ticks " + ticks.joined(separator: " "))
    }

    // MARK: - Haptic check (foreground)

    /// The two beacon patterns of the spec: done = success; attention = click, ≥ 100 ms, click.
    func hapticCheck() async {
        note("haptics: done pattern, then attention pattern")
        WKInterfaceDevice.current().play(.success)
        try? await Task.sleep(nanoseconds: 1_200_000_000)
        WKInterfaceDevice.current().play(.click)
        try? await Task.sleep(nanoseconds: 250_000_000)
        WKInterfaceDevice.current().play(.click)
    }

    // MARK: - Plumbing

    private func ping(_ path: ProbePath) async -> Result<Double, Error> {
        var request = URLRequest(url: path.base.appendingPathComponent("api/ping"))
        request.setValue("Bearer \(Secrets.token)", forHTTPHeaderField: "Authorization")
        let started = Date()
        do {
            let (_, response) = try await session.data(for: request)
            let ms = Date().timeIntervalSince(started) * 1000
            guard let http = response as? HTTPURLResponse else { return .failure(ProbeError.notHTTP) }
            guard http.statusCode == 200 else { return .failure(ProbeError.status(http.statusCode)) }
            return .success(ms)
        } catch {
            return .failure(error)
        }
    }

    private func firstReachable() async -> ProbePath? {
        for path in paths {
            if case .success = await ping(path) { return path }
        }
        return nil
    }

    /// POSTs `text` to /api/log on every path; logs which paths delivered it.
    private func report(_ text: String) async {
        var delivered: [String] = []
        for path in paths {
            var request = URLRequest(url: path.base.appendingPathComponent("api/log"))
            request.httpMethod = "POST"
            request.setValue("Bearer \(Secrets.token)", forHTTPHeaderField: "Authorization")
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try? JSONSerialization.data(withJSONObject: ["text": "[\(path.id)] " + text])
            if let answer = try? await session.data(for: request), (answer.1 as? HTTPURLResponse)?.statusCode == 200 {
                delivered.append(path.label)
            }
        }
        note(delivered.isEmpty ? "report: not delivered" : "report → " + delivered.joined(separator: ", "))
    }

    private func note(_ text: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        lines.insert("\(stamp) \(text)", at: 0)
        if lines.count > 60 { lines.removeLast() }
    }

    private func percentile(_ values: [Double], _ p: Double) -> String {
        guard !values.isEmpty else { return "–" }
        let sorted = values.sorted()
        let index = min(sorted.count - 1, Int((Double(sorted.count - 1) * p).rounded()))
        return String(format: "%.0f ms", sorted[index])
    }

    private func describe(_ error: Error) -> String {
        if let probe = error as? ProbeError { return probe.label }
        let ns = error as NSError
        return "\(ns.domain.replacingOccurrences(of: "NSURLErrorDomain", with: "URL"))\(ns.code)"
    }

    private func stateName(_ state: WKApplicationState) -> String {
        switch state {
        case .active: return "A"
        case .inactive: return "I"
        case .background: return "B"
        @unknown default: return "?"
        }
    }

    private func resultMark(_ result: Result<Double, Error>) -> String {
        switch result {
        case .success(let ms): return String(format: "%.0f", ms)
        case .failure(let error): return "✗" + describe(error)
        }
    }
}

enum ProbeError: Error {
    case notHTTP
    case status(Int)

    var label: String {
        switch self {
        case .notHTTP: return "notHTTP"
        case .status(let code): return "HTTP\(code)"
        }
    }
}

/// A mindfulness extended runtime session (Info.plist WKBackgroundModes = mindfulness):
/// frontmost, up to 1 hour. Apple: the app may "play sounds or haptics, even after the watch's
/// screen turns off". Measurement 5 checks that on the wrist.
final class ExtendedSession: NSObject, WKExtendedRuntimeSessionDelegate {
    var onLog: (String) -> Void = { _ in }
    private var session: WKExtendedRuntimeSession?

    func start() {
        let session = WKExtendedRuntimeSession()
        session.delegate = self
        session.start()
        self.session = session
    }

    func stop() {
        session?.invalidate()
        session = nil
    }

    func extendedRuntimeSessionDidStart(_ extendedRuntimeSession: WKExtendedRuntimeSession) {
        onLog("session started, expires \(extendedRuntimeSession.expirationDate?.formatted(date: .omitted, time: .shortened) ?? "?")")
    }

    func extendedRuntimeSessionWillExpire(_ extendedRuntimeSession: WKExtendedRuntimeSession) {
        onLog("session will expire")
    }

    func extendedRuntimeSession(_ extendedRuntimeSession: WKExtendedRuntimeSession,
                                didInvalidateWith reason: WKExtendedRuntimeSessionInvalidationReason,
                                error: Error?) {
        onLog("session invalidated: reason \(reason.rawValue)\(error.map { " · \($0.localizedDescription)" } ?? "")")
    }
}
