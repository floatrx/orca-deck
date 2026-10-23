/// The beacon of the spec: one pattern per batch of events. `attention` (2 haptics) beats `done` (1).
public enum Beacon: Equatable, Sendable {
    case none, done, attention

    public static func pattern(for events: [BridgeEvent]) -> Beacon {
        if events.contains(where: { $0.kind == .attention }) { return .attention }
        return events.isEmpty ? .none : .done
    }
}
