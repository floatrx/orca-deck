import Foundation

/// orca-deck protocol v1 (protocol.md). Lua drops nil keys, so every nullable field is optional:
/// "absent means null".
public enum ProtocolVersion {
    public static let supported = 1
}

public enum ProtocolError: Error, Equatable {
    case unsupportedVersion(Int)
}

/// protocol.md § Buckets. An unknown value falls back to `idle`, so a newer bridge cannot break decoding.
public enum Bucket: String, Codable, Sendable {
    case working, done, attention, idle

    public init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = Bucket(rawValue: raw) ?? .idle
    }
}

public struct Agent: Codable, Equatable, Identifiable, Sendable {
    public let id: String
    public let kind: String?
    public let chat: Bool?
    public let state: String?
    public let bucket: Bucket
    public let interrupted: Bool?
    public let tool: String?
    public let prompt: String?
    public let last: String?
    /// When the agent entered `state`, epoch ms.
    public let since: Double?
}

public struct Worktree: Codable, Equatable, Identifiable, Sendable {
    public let id: String
    public let name: String
    public let repo: String?
    public let branch: String?
    public let active: Bool
    public let status: String?
    /// The agent that most needs the user; nil when the worktree has no agents.
    public let lead: Agent?
    public let agents: [Agent]
}

public enum EventKind: String, Codable, Sendable {
    case done, attention
}

public struct BridgeEvent: Codable, Equatable, Sendable {
    public let seq: Int64
    public let kind: EventKind
    public let worktreeId: String
    public let worktree: String
    public let agentId: String
    public let at: Double

    public init(seq: Int64, kind: EventKind, worktreeId: String, worktree: String, agentId: String, at: Double) {
        self.seq = seq
        self.kind = kind
        self.worktreeId = worktreeId
        self.worktree = worktree
        self.agentId = agentId
        self.at = at
    }
}

/// `GET /api/state?since=<seq>`.
public struct StateResponse: Codable, Equatable, Sendable {
    public let v: Int
    public let seq: Int64
    public let worktrees: [Worktree]
    public let events: [BridgeEvent]

    /// Checks the protocol version first, then decodes: a newer bridge means "update the app", not a
    /// guess — even when its JSON no longer has the v1 shape.
    public static func decode(_ data: Data) throws -> StateResponse {
        let decoder = JSONDecoder()
        let envelope = try decoder.decode(Envelope.self, from: data)
        guard envelope.v <= ProtocolVersion.supported else { throw ProtocolError.unsupportedVersion(envelope.v) }
        return try decoder.decode(StateResponse.self, from: data)
    }

    private struct Envelope: Decodable {
        let v: Int
    }
}

/// The reply to `POST /api/cmd`.
public struct Ack: Codable, Equatable, Sendable {
    public let cmd: String
    public let ok: Bool
    public let note: String?
}

/// The body of `POST /api/cmd`. Nil fields are left out of the JSON.
public struct Command: Encodable, Equatable, Sendable {
    public let cmd: String
    public let id: String?
    public let pane: String?
    public var text: String? = nil

    /// Focus `worktree` in Orca: its lead agent's tab when it has one, else the worktree itself.
    public static func go(_ worktree: Worktree) -> Command {
        Command(cmd: "go", id: worktree.id, pane: worktree.lead?.id)
    }

    /// Focus one agent's tab in `worktree`.
    public static func go(_ worktree: Worktree, agent: Agent) -> Command {
        Command(cmd: "go", id: worktree.id, pane: agent.id)
    }

    /// Send `text` to a chat agent as a user prompt (protocol.md § say).
    public static func say(_ agentId: String, _ text: String) -> Command {
        Command(cmd: "say", id: agentId, pane: nil, text: text)
    }
}

/// The outcome of the last `say` for an agent; `ok` is nil while it is still sending.
public struct SendOutcome: Codable, Equatable, Sendable {
    public let at: Double?
    public let ok: Bool?
    public let note: String?
}

/// `GET /api/agent?id=` (protocol.md § AgentDetail).
public struct AgentDetail: Codable, Equatable, Sendable {
    public let id: String
    public let worktreeId: String
    public let worktree: String
    public let kind: String?
    public let chat: Bool?
    public let state: String?
    public let bucket: Bucket
    public let since: Double?
    /// The full last assistant message without its trailing Next lines.
    public let message: String
    /// False while `message` is still Orca's 200-character cut.
    public let full: Bool
    /// The pending Next variants; empty when none or already used.
    public let next: [String]
    public let send: SendOutcome?
}
