import ApexCore
import Foundation

/// The bridge's remote HTTP API (protocol.md § HTTP): `GET /api/state`, `POST /api/cmd`. It knows several
/// routes to the bridge (Routes): the LAN address the bridge advertised last, the LAN IP from the build, the
/// HTTPS tunnel. It uses the one that worked last; a network failure moves it to the next, and it tries its way
/// back to the LAN every minute — at once when the bridge advertises a new LAN address.
@MainActor
final class BridgeClient {
    /// The routes from the build (watch/configure.sh).
    let configured: [URL]
    let token: String
    /// The LAN URL the bridge advertised last (StateResponse.lan), kept across launches: after a move to another
    /// network the tunnel carries the first state, and from then on the LAN does.
    private(set) var learned: URL?
    /// The route that answered last.
    private var answered: URL?
    private var lastProbe: Date?
    private let session: URLSession
    private static let learnedKey = "apex.learnedLAN"

    var routes: [URL] { Routes.merge(learned: learned, configured: configured) }
    var currentURL: URL {
        let list = routes
        return answered.flatMap { list.contains($0) ? $0 : nil } ?? list[0]
    }

    init(routes: [URL], token: String) {
        precondition(!routes.isEmpty, "Apex needs at least one route to the bridge")
        self.configured = routes
        self.token = token
        learned = UserDefaults.standard.string(forKey: Self.learnedKey).flatMap(URL.init(string:))
        let config = URLSessionConfiguration.ephemeral
        // Each request sets its own timeout per route (Routes.timeout); this is the ceiling.
        config.timeoutIntervalForRequest = 6
        config.timeoutIntervalForResource = 6
        config.waitsForConnectivity = false
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: config)
    }

    func state(since: Int64?) async throws -> StateResponse {
        let query = since.map { [URLQueryItem(name: "since", value: String($0))] } ?? []
        let response = try StateResponse.decode(try await send(path: "api/state", query: query))
        learn(response.lan)
        return response
    }

    /// Remembers the LAN address the bridge advertises; a new one is tried first on the very next request.
    private func learn(_ lan: [String]?) {
        guard let url = Routes.learnable(lan), url != learned else { return }
        learned = url
        UserDefaults.standard.set(url.absoluteString, forKey: Self.learnedKey)
        lastProbe = nil
    }

    /// `GET /api/agent?id=` — the agent page's message, Next variants and last send outcome.
    func agent(id: String) async throws -> AgentDetail {
        let data = try await send(path: "api/agent", query: [URLQueryItem(name: "id", value: id)])
        return try JSONDecoder().decode(AgentDetail.self, from: data)
    }

    func send(_ command: Command) async throws -> Ack {
        try JSONDecoder().decode(Ack.self, from: try await send(path: "api/cmd", body: try JSONEncoder().encode(command)))
    }

    /// `POST /api/log`: one line in the bridge trail — how the Mac sees what the watch is doing.
    func log(_ text: String) async throws {
        _ = try await send(path: "api/log", body: try JSONSerialization.data(withJSONObject: ["text": text]))
    }

    /// One request, route by route (Routes.order) until one answers; the route that answered becomes current.
    /// A bridge answer (any HTTP status) ends the walk — only a network failure tries the next route.
    private func send(path: String, query: [URLQueryItem] = [], body: Data? = nil) async throws -> Data {
        let list = routes
        let current = list.firstIndex(of: currentURL) ?? 0
        let plan = Routes.order(current: current, count: list.count, now: Date(), lastProbe: lastProbe)
        if plan.probe { lastProbe = Date() }
        // Requests overlap (poll, detail, log): a slow one through the tunnel may finish after another moved
        // to the LAN. It may only set the route if nobody changed it meanwhile, or if its route is better.
        let startedWith = answered
        var failure: Error = URLError(.cannotConnectToHost)
        // A gateway status says the bridge is down behind the tunnel — the most telling failure, kept for the end.
        var gateway: Error?
        for index in plan.order {
            do {
                let data = try await attempt(list[index], path: path, query: query, body: body)
                let now = routes
                let rank = { (url: URL?) in url.flatMap { now.firstIndex(of: $0) } ?? Int.max }
                if answered == startedWith || rank(list[index]) < rank(currentURL) { answered = list[index] }
                return data
            } catch where Routes.isRouteFailure(error) {
                failure = error
            } catch BridgeError.status(let code) where Routes.isGatewayStatus(code) {
                gateway = BridgeError.status(code)
            }
        }
        throw gateway ?? failure
    }

    private func attempt(_ base: URL, path: String, query: [URLQueryItem], body: Data?) async throws -> Data {
        var components = URLComponents(url: base.appendingPathComponent(path), resolvingAgainstBaseURL: false)
        if !query.isEmpty { components?.queryItems = query }
        guard let url = components?.url else { throw URLError(.badURL) }
        var request = URLRequest(url: url, timeoutInterval: Routes.timeout(for: base))
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        if let body {
            request.httpMethod = "POST"
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = body
        }
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        guard http.statusCode == 200 else { throw BridgeError.status(http.statusCode) }
        return data
    }
}

enum BridgeError: Error {
    case status(Int)
}
