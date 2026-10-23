import ApexCore
import Foundation

/// The bridge's remote HTTP API (protocol.md § HTTP): `GET /api/state`, `POST /api/cmd`.
struct BridgeClient {
    let baseURL: URL
    let token: String
    private let session: URLSession

    init(baseURL: URL, token: String) {
        self.baseURL = baseURL
        self.token = token
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 4
        config.timeoutIntervalForResource = 6
        config.waitsForConnectivity = false
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: config)
    }

    func state(since: Int64?) async throws -> StateResponse {
        var components = URLComponents(url: baseURL.appendingPathComponent("api/state"), resolvingAgainstBaseURL: false)
        if let since { components?.queryItems = [URLQueryItem(name: "since", value: String(since))] }
        guard let url = components?.url else { throw URLError(.badURL) }
        return try StateResponse.decode(try await send(URLRequest(url: url)))
    }

    /// `GET /api/agent?id=` — the agent page's message, Next variants and last send outcome.
    func agent(id: String) async throws -> AgentDetail {
        var components = URLComponents(url: baseURL.appendingPathComponent("api/agent"), resolvingAgainstBaseURL: false)
        components?.queryItems = [URLQueryItem(name: "id", value: id)]
        guard let url = components?.url else { throw URLError(.badURL) }
        return try JSONDecoder().decode(AgentDetail.self, from: try await send(URLRequest(url: url)))
    }

    func send(_ command: Command) async throws -> Ack {
        var request = URLRequest(url: baseURL.appendingPathComponent("api/cmd"))
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(command)
        return try JSONDecoder().decode(Ack.self, from: try await send(request))
    }

    /// `POST /api/log`: one line in the bridge trail — how the Mac sees what the watch is doing.
    func log(_ text: String) async throws {
        var request = URLRequest(url: baseURL.appendingPathComponent("api/log"))
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: ["text": text])
        _ = try await send(request)
    }

    private func send(_ request: URLRequest) async throws -> Data {
        var request = request
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        guard http.statusCode == 200 else { throw BridgeError.status(http.statusCode) }
        return data
    }
}

enum BridgeError: Error {
    case status(Int)
}
