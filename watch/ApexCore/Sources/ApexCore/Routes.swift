import Foundation

/// The ways to the bridge, fastest first: the Mac's Bonjour name on the LAN, its LAN IP, then the HTTPS tunnel
/// (protocol.md § Transports). The client sticks to the route that worked last; while it is off the first
/// route it tries the first one again every `probeInterval`, so it comes home to the LAN by itself.
public enum Routes {
    public static let probeInterval: TimeInterval = 60

    /// The order to try the routes in: the current one first, then the rest by priority. Off the first route,
    /// the first one goes ahead once per `probeInterval` (`probe` is true then: the caller records the time).
    public static func order(current: Int, count: Int, now: Date, lastProbe: Date?) -> (order: [Int], probe: Bool) {
        guard count > 0 else { return ([], false) }
        let current = min(max(current, 0), count - 1)
        let rest = (0..<count).filter { $0 != current }
        if current != 0, now.timeIntervalSince(lastProbe ?? .distantPast) >= probeInterval {
            return ([0] + [current] + rest.filter { $0 != 0 }, true)
        }
        return ([current] + rest, false)
    }

    /// A failure that another route may not have: the host is unreachable, unknown or slow. An HTTP status
    /// (a wrong token, a bad request) comes from the bridge itself — the same on every route.
    public static func isRouteFailure(_ error: Error) -> Bool {
        guard let url = error as? URLError else { return false }
        switch url.code {
        case .timedOut, .cannotFindHost, .cannotConnectToHost, .networkConnectionLost, .dnsLookupFailed,
             .notConnectedToInternet, .secureConnectionFailed, .cannotLoadFromNetwork, .internationalRoamingOff,
             .dataNotAllowed, .resourceUnavailable:
            return true
        default:
            return false
        }
    }

    /// An HTTP status that a gateway (the Cloudflare tunnel) sends when it cannot reach the bridge behind it —
    /// the bridge itself never answers with one. Another route may still reach the bridge.
    public static func isGatewayStatus(_ code: Int) -> Bool {
        [502, 503, 504].contains(code) || (520...530).contains(code)
    }

    /// How long one try may take: a LAN route answers in ~70 ms or not at all, so it gives up fast; the tunnel
    /// crosses the internet (~190 ms, more on a bad link).
    public static func timeout(for url: URL) -> TimeInterval {
        isLocal(url) ? 2.5 : 6
    }

    /// A Bonjour name, a private IP or localhost — a route that only works on the same network.
    public static func isLocal(_ url: URL) -> Bool {
        guard let host = url.host?.lowercased() else { return false }
        if host == "localhost" || host.hasSuffix(".local") { return true }
        let parts = host.split(separator: ".").compactMap { Int($0) }
        guard parts.count == 4 else { return false }
        return parts[0] == 10 || parts[0] == 127 || (parts[0] == 192 && parts[1] == 168)
            || (parts[0] == 172 && (16...31).contains(parts[1]))
    }

    /// The routes to walk: the LAN URL learned from the bridge first (the Mac's address on today's network), then
    /// the configured ones, without duplicates. The learned one makes a new network work at LAN speed without a
    /// reinstall; the configured tunnel keeps everything reachable meanwhile.
    public static func merge(learned: URL?, configured: [URL]) -> [URL] {
        var seen = Set<String>()
        return ([learned].compactMap { $0 } + configured).filter { seen.insert(key($0)).inserted }
    }

    /// The URL the bridge advertises for its LAN address: the first well-formed http one.
    public static func learnable(_ lan: [String]?) -> URL? {
        (lan ?? []).lazy.compactMap(URL.init(string:)).first { $0.scheme == "http" && isLocal($0) }
    }

    private static func key(_ url: URL) -> String {
        var text = url.absoluteString.lowercased()
        while text.hasSuffix("/") { text.removeLast() }
        return text
    }

    /// "LAN" or "tunnel", for Settings and the trail.
    public static func label(_ url: URL) -> String {
        isLocal(url) ? "LAN" : "tunnel"
    }
}
