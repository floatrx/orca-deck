/// The client loop of protocol.md § HTTP: remembers the newest event seq and hands out only the
/// events the watch has not pulsed for yet.
public struct EventCursor: Equatable, Sendable {
    /// The value for `?since=`; nil before the first response.
    public private(set) var since: Int64?

    public init() {}

    /// Takes one `/api/state` response and returns the events to pulse for, oldest first.
    /// The first response only stores `seq` (a fresh client replays no pulses). A bridge `seq`
    /// behind the cursor means a restarted bridge: the cursor adopts it.
    public mutating func accept(seq: Int64, events: [BridgeEvent]) -> [BridgeEvent] {
        guard let last = since else {
            since = seq
            return []
        }
        if seq < last {
            since = seq
            return []
        }
        let fresh = events.filter { $0.seq > last }.sorted { $0.seq < $1.seq }
        since = max(seq, fresh.last?.seq ?? last)
        return fresh
    }
}
