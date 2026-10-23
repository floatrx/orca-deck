import XCTest
@testable import ApexCore

final class ProtocolDecodingTests: XCTestCase {
    private func fixture() throws -> StateResponse {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "state", withExtension: "json", subdirectory: "Fixtures"))
        return try StateResponse.decode(Data(contentsOf: url))
    }

    func testDecodesTheBridgeStateResponse() throws {
        let state = try fixture()
        XCTAssertEqual(state.v, 1)
        XCTAssertEqual(state.seq, 1792702308450)
        XCTAssertEqual(state.worktrees.map(\.name), ["master", "main", "odd"])
        XCTAssertEqual(state.events.map(\.kind), [.done, .attention])
    }

    func testAbsentLeadMeansNoAgents() throws {
        let empty = try fixture().worktrees[0]
        XCTAssertNil(empty.lead)
        XCTAssertTrue(empty.agents.isEmpty)
    }

    func testAgentFieldsAndAbsentOptionals() throws {
        let deck = try fixture().worktrees[1]
        XCTAssertTrue(deck.active)
        XCTAssertEqual(deck.lead?.bucket, .attention)
        XCTAssertEqual(deck.agents[0].tool, "Bash")
        XCTAssertNil(deck.agents[1].tool, "an absent `tool` decodes as nil")
        XCTAssertEqual(deck.agents[0].since, 1792702950000)
    }

    func testUnknownBucketFallsBackToIdle() throws {
        let odd = try fixture().worktrees[2]
        XCTAssertEqual(odd.lead?.bucket, .idle)
        XCTAssertNil(odd.lead?.kind)
    }

    /// Decodes a real bridge response, kept out of the repo because it holds prompt text:
    /// `curl -H "Authorization: Bearer $TOKEN" "localhost:7352/api/state?since=0" > /tmp/s.json`
    /// `APEX_LIVE_STATE=/tmp/s.json swift test`
    func testDecodesALiveBridgeResponseWhenProvided() throws {
        guard let path = ProcessInfo.processInfo.environment["APEX_LIVE_STATE"] else {
            throw XCTSkip("set APEX_LIVE_STATE to a saved /api/state response")
        }
        let state = try StateResponse.decode(Data(contentsOf: URL(fileURLWithPath: path)))
        XCTAssertFalse(state.worktrees.isEmpty)
    }

    func testRejectsANewerProtocolVersion() {
        let json = Data(#"{"v":2,"seq":1,"worktrees":[],"events":[]}"#.utf8)
        XCTAssertThrowsError(try StateResponse.decode(json)) { error in
            XCTAssertEqual(error as? ProtocolError, .unsupportedVersion(2))
        }
    }

    func testANewerVersionWithANewShapeStillSaysUpdate() {
        // A v2 bridge may rename fields; the version must be read before the v1 shape is required.
        let json = Data(#"{"v":2,"cursor":"abc","items":[]}"#.utf8)
        XCTAssertThrowsError(try StateResponse.decode(json)) { error in
            XCTAssertEqual(error as? ProtocolError, .unsupportedVersion(2))
        }
    }

    func testDecodesAnAck() throws {
        let ack = try JSONDecoder().decode(Ack.self, from: Data(#"{"type":"ack","v":1,"cmd":"go","ok":false,"note":"Orca is not running"}"#.utf8))
        XCTAssertEqual(ack.cmd, "go")
        XCTAssertFalse(ack.ok)
        XCTAssertEqual(ack.note, "Orca is not running")
    }

    func testGoCommandCarriesTheLeadPane() throws {
        let deck = try fixture().worktrees[1]
        let body = try JSONSerialization.jsonObject(with: JSONEncoder().encode(Command.go(deck))) as? [String: String]
        XCTAssertEqual(body, ["cmd": "go", "id": "w-deck::/Users/x/Projects/orca-deck", "pane": "structured-agent-session-claude_1:aaa"])
    }

    func testGoCommandWithoutAgentsOmitsThePane() throws {
        let empty = try fixture().worktrees[0]
        let body = try JSONSerialization.jsonObject(with: JSONEncoder().encode(Command.go(empty))) as? [String: String]
        XCTAssertEqual(body, ["cmd": "go", "id": "w-empty::/Users/x/orca/projects/generic"])
    }
}

final class EventCursorTests: XCTestCase {
    private func event(_ seq: Int64, _ kind: EventKind) -> BridgeEvent {
        BridgeEvent(seq: seq, kind: kind, worktreeId: "w", worktree: "main", agentId: "a", at: 0)
    }

    func testFirstResponseStoresSeqAndPulsesNothing() {
        var cursor = EventCursor()
        XCTAssertNil(cursor.since)
        let fresh = cursor.accept(seq: 100, events: [event(99, .done)])
        XCTAssertTrue(fresh.isEmpty, "a fresh client replays no pulses")
        XCTAssertEqual(cursor.since, 100)
    }

    func testLaterResponsesReturnOnlyNewEvents() {
        var cursor = EventCursor()
        _ = cursor.accept(seq: 100, events: [])
        let fresh = cursor.accept(seq: 102, events: [event(100, .done), event(101, .done), event(102, .attention)])
        XCTAssertEqual(fresh.map(\.seq), [101, 102])
        XCTAssertEqual(cursor.since, 102)
    }

    func testRepeatedEventsAreSkipped() {
        var cursor = EventCursor()
        _ = cursor.accept(seq: 100, events: [])
        _ = cursor.accept(seq: 101, events: [event(101, .done)])
        XCTAssertTrue(cursor.accept(seq: 101, events: [event(101, .done)]).isEmpty)
    }

    func testABridgeBehindTheCursorResetsIt() {
        var cursor = EventCursor()
        _ = cursor.accept(seq: 500, events: [])
        XCTAssertTrue(cursor.accept(seq: 300, events: []).isEmpty)
        XCTAssertEqual(cursor.since, 300, "protocol.md client loop step 3: adopt the bridge's seq")
    }
}

final class BeaconTests: XCTestCase {
    private func event(_ kind: EventKind) -> BridgeEvent {
        BridgeEvent(seq: 1, kind: kind, worktreeId: "w", worktree: "main", agentId: "a", at: 0)
    }

    func testNoEventsNoPulse() {
        XCTAssertEqual(Beacon.pattern(for: []), .none)
    }

    func testDoneIsOnePulse() {
        XCTAssertEqual(Beacon.pattern(for: [event(.done), event(.done)]), .done)
    }

    func testAnyAttentionWins() {
        XCTAssertEqual(Beacon.pattern(for: [event(.done), event(.attention)]), .attention)
    }
}

final class FormattingTests: XCTestCase {
    func testAgeRoundsLikeTheMock() {
        let now: Double = 10_000_000
        XCTAssertEqual(Format.age(sinceMs: now - 4_000, nowMs: now), "4s")
        XCTAssertEqual(Format.age(sinceMs: now - 125_000, nowMs: now), "2m")
        XCTAssertEqual(Format.age(sinceMs: now - 3 * 3_600_000, nowMs: now), "3h")
        XCTAssertEqual(Format.age(sinceMs: now - 2 * 86_400_000, nowMs: now), "2d")
        XCTAssertEqual(Format.age(sinceMs: nil, nowMs: now), "")
        XCTAssertEqual(Format.age(sinceMs: now + 5_000, nowMs: now), "0s", "a clock ahead never shows a negative age")
    }

    func testToolLabelDropsTheMcpPrefix() {
        XCTAssertEqual(Format.tool("mcp__plugin_playwright_playwright__browser_take_screenshot"), "browser_take_screenshot")
        XCTAssertEqual(Format.tool("Bash"), "Bash")
    }

    func testSubtitleShowsTheRepoOnlyWhenItDiffersFromTheName() throws {
        let decoder = JSONDecoder()
        let same = try decoder.decode(Worktree.self, from: Data(#"{"id":"1","name":"orca-deck","repo":"orca-deck","active":false,"agents":[]}"#.utf8))
        let differs = try decoder.decode(Worktree.self, from: Data(#"{"id":"2","name":"main","repo":"orca-deck","active":false,"agents":[]}"#.utf8))
        XCTAssertNil(Format.repoSubtitle(same))
        XCTAssertEqual(Format.repoSubtitle(differs), "orca-deck")
    }
}

final class SelectionTests: XCTestCase {
    private func worktrees(_ ids: [String], active: String? = nil) throws -> [Worktree] {
        try ids.map { id in
            try JSONDecoder().decode(Worktree.self, from: Data(#"{"id":"\#(id)","name":"\#(id)","active":\#(id == active),"agents":[]}"#.utf8))
        }
    }

    func testNoSelectionYetStartsOnTheActiveWorktree() throws {
        XCTAssertEqual(Selection.index(of: nil, previous: 0, in: try worktrees(["a", "b", "c"], active: "c")), 2)
    }

    func testNoSelectionAndNoActiveStartsAtTheTop() throws {
        XCTAssertEqual(Selection.index(of: nil, previous: 0, in: try worktrees(["a", "b"])), 0)
    }

    func testTheSelectionFollowsItsWorktreeWhenTheListReorders() throws {
        XCTAssertEqual(Selection.index(of: "b", previous: 1, in: try worktrees(["c", "a", "b"])), 2)
    }

    func testAVanishedSelectionKeepsThePositionClamped() throws {
        XCTAssertEqual(Selection.index(of: "gone", previous: 1, in: try worktrees(["a", "b", "c"])), 1)
        XCTAssertEqual(Selection.index(of: "gone", previous: 5, in: try worktrees(["a", "b"])), 1)
    }

    func testAnEmptyListGivesZero() throws {
        XCTAssertEqual(Selection.index(of: "a", previous: 3, in: []), 0)
    }

    func testHidingInactiveDropsTheWorktreesWithoutAgents() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "state", withExtension: "json", subdirectory: "Fixtures"))
        let all = try StateResponse.decode(Data(contentsOf: url)).worktrees
        XCTAssertEqual(Selection.visible(all, hideInactive: true).map(\.name), ["main", "odd"])
        XCTAssertEqual(Selection.visible(all, hideInactive: false).map(\.name), ["master", "main", "odd"])
    }

    func testTheWindowCentresTheSelection() {
        XCTAssertEqual(Selection.window(selected: 5, count: 10, size: 5), 3..<8)
    }

    func testTheWindowSlidesAtTheEdges() {
        XCTAssertEqual(Selection.window(selected: 0, count: 10, size: 5), 0..<5)
        XCTAssertEqual(Selection.window(selected: 9, count: 10, size: 5), 5..<10)
    }

    func testTheWindowShrinksToAShortList() {
        XCTAssertEqual(Selection.window(selected: 1, count: 3, size: 5), 0..<3)
        XCTAssertEqual(Selection.window(selected: 0, count: 0, size: 5), 0..<0)
    }
}
