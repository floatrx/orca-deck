import XCTest
@testable import ApexCore

final class AgentDetailTests: XCTestCase {
    func testDecodesTheAgentDetail() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "agent", withExtension: "json", subdirectory: "Fixtures"))
        let detail = try JSONDecoder().decode(AgentDetail.self, from: Data(contentsOf: url))
        XCTAssertEqual(detail.worktree, "main")
        XCTAssertTrue(detail.full)
        XCTAssertEqual(detail.next, ["commit and push", "run the e2e suite"])
        XCTAssertEqual(detail.send?.ok, true)
        XCTAssertEqual(detail.bucket, .done)
    }

    func testASendStillRunningHasNoOk() throws {
        let json = Data(#"{"v":1,"id":"a","worktreeId":"w","worktree":"m","bucket":"working","message":"","full":false,"next":[],"send":{"at":1,"note":"sending"}}"#.utf8)
        let detail = try JSONDecoder().decode(AgentDetail.self, from: json)
        XCTAssertNil(detail.send?.ok)
        XCTAssertEqual(detail.send?.note, "sending")
        XCTAssertNil(detail.chat)
    }

    func testDecodesAPendingQuestionAndTheLastAnswer() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "agent-question", withExtension: "json", subdirectory: "Fixtures"))
        let detail = try JSONDecoder().decode(AgentDetail.self, from: Data(contentsOf: url))
        let question = try XCTUnwrap(detail.question)
        XCTAssertEqual(question.text, "Which colour do you prefer?")
        XCTAssertEqual(question.header, "Colour")
        XCTAssertEqual(question.revision, 1)
        XCTAssertEqual(question.options.map(\.label), ["red", "green"])
        XCTAssertTrue(question.answerable)
        XCTAssertEqual(detail.answer?.ok, true)
        XCTAssertEqual(detail.bucket, .attention)
    }

    func testADetailWithoutAQuestionStillDecodes() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "agent", withExtension: "json", subdirectory: "Fixtures"))
        let detail = try JSONDecoder().decode(AgentDetail.self, from: Data(contentsOf: url))
        XCTAssertNil(detail.question)
        XCTAssertNil(detail.answer)
    }

    func testAnswerCommandCarriesTheQuestionAndTheOption() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "agent-question", withExtension: "json", subdirectory: "Fixtures"))
        let question = try XCTUnwrap(try JSONDecoder().decode(AgentDetail.self, from: Data(contentsOf: url)).question)
        let command = Command.answer("a:1", question, option: question.options[1])
        let body = try XCTUnwrap(try JSONSerialization.jsonObject(with: JSONEncoder().encode(command)) as? [String: Any])
        XCTAssertEqual(body["cmd"] as? String, "answer")
        XCTAssertEqual(body["id"] as? String, "a:1")
        XCTAssertEqual(body["itemId"] as? String, question.itemId)
        XCTAssertEqual(body["revision"] as? Int, 1)
        XCTAssertEqual(body["optionId"] as? String, "q1:choice-2")
        XCTAssertNil(body["text"], "an answer carries no text")
    }

    func testSayCommandCarriesTheAgentAndText() throws {
        let body = try JSONSerialization.jsonObject(with: JSONEncoder().encode(Command.say("a:1", "hi"))) as? [String: String]
        XCTAssertEqual(body, ["cmd": "say", "id": "a:1", "text": "hi"])
    }
}

final class DialTests: XCTestCase {
    func testDotsSpreadEvenlyFromTwelveOClock() {
        XCTAssertEqual(Dial.angle(index: 0, count: 4), 0)
        XCTAssertEqual(Dial.angle(index: 1, count: 4), 90)
        XCTAssertEqual(Dial.angle(index: 3, count: 4), 270)
        XCTAssertEqual(Dial.angle(index: 0, count: 0), 0)
    }

    func testTheDialTurnsTheChosenDotToTwelve() {
        XCTAssertEqual(Dial.rotation(selected: 1, count: 4, current: 0), -90)
    }

    func testTheDialTakesTheShortWayRound() {
        // From the last dot (-270°) to the first: +90° on, not +270° back — like aimDots in the mock.
        XCTAssertEqual(Dial.rotation(selected: 0, count: 4, current: -270), -360)
        XCTAssertEqual(Dial.rotation(selected: 3, count: 4, current: 0), 90)
    }
}

final class CentreTests: XCTestCase {
    private func worktree(_ json: String) throws -> Worktree {
        try JSONDecoder().decode(Worktree.self, from: Data(json.utf8))
    }

    private let deckJSON = #"{"id":"w","name":"main","repo":"orca-deck","active":true,"lead":{"id":"b","kind":"claude","chat":true,"state":"blocked","bucket":"attention","last":"Which transport?","since":1000},"agents":[{"id":"a","kind":"claude","chat":false,"state":"working","bucket":"working","tool":"mcp__x__browser_click","last":"clicking","since":2000},{"id":"b","kind":"claude","chat":true,"state":"blocked","bucket":"attention","last":"Which transport?","since":1000}]}"#

    func testSummaryCountsEveryAgentLikeTheMock() throws {
        let deck = try worktree(deckJSON)
        let idle = try worktree(#"{"id":"x","name":"x","active":false,"agents":[]}"#)
        XCTAssertEqual(Centre.summary([deck, idle]), "1 waiting · 1 working")
        XCTAssertEqual(Centre.summary([idle]), "all quiet")
        XCTAssertEqual(Centre.summary([]), "no workspaces")
    }

    func testStateLabel() throws {
        let deck = try worktree(deckJSON)
        XCTAssertEqual(Centre.state(deck.lead), "NEEDS YOU")
        XCTAssertEqual(Centre.state(deck.agents[0]), "WORKING")
        XCTAssertEqual(Centre.state(nil), "NO AGENTS")
    }

    func testMetaAndDetail() throws {
        let deck = try worktree(deckJSON)
        XCTAssertEqual(Centre.meta(deck, agent: deck.agents[1]), "orca-deck · chat · claude")
        XCTAssertEqual(Centre.detail(deck.agents[0]), "▸ browser_click — clicking")
        XCTAssertEqual(Centre.detail(deck.agents[1]), "Which transport?")
    }

    func testCounter() throws {
        let deck = try worktree(deckJSON)
        XCTAssertEqual(Centre.counter(index: 1, count: 8, worktree: deck, agent: deck.agents[1], nowMs: 121_000), "2/8 · agent 2/2 · 2m")
    }

    func testPickedAgentAndNextAgent() throws {
        let deck = try worktree(deckJSON)
        XCTAssertEqual(Agents.shown(deck, picked: nil)?.id, "b", "the lead by default")
        XCTAssertEqual(Agents.shown(deck, picked: "a")?.id, "a")
        XCTAssertEqual(Agents.shown(deck, picked: "gone")?.id, "b")
        XCTAssertEqual(Agents.next(after: "b", in: deck), "a")
        XCTAssertEqual(Agents.next(after: "a", in: deck), "b")
    }
}

final class TrackTests: XCTestCase {
    private let rect = CGRect(x: 0, y: 0, width: 200, height: 240)

    private func assertPoint(_ p: CGPoint, _ x: CGFloat, _ y: CGFloat, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(p.x, x, accuracy: 0.5, file: file, line: line)
        XCTAssertEqual(p.y, y, accuracy: 0.5, file: file, line: line)
    }

    func testTheTrackStartsAtTopCentreAndRunsClockwise() {
        assertPoint(Track.point(at: 0, in: rect, radius: 30), 100, 0)
        assertPoint(Track.point(at: 0.25, in: rect, radius: 30), 200, 120)
        assertPoint(Track.point(at: 0.5, in: rect, radius: 30), 100, 240)
        assertPoint(Track.point(at: 0.75, in: rect, radius: 30), 0, 120)
    }

    func testTheTrackWrapsRound() {
        assertPoint(Track.point(at: 1, in: rect, radius: 30), 100, 0)
        assertPoint(Track.point(at: -0.25, in: rect, radius: 30), 0, 120)
    }

    func testACornerPointLiesOnTheArc() {
        // Halfway round the top-right corner: on the arc, radius 30 from its centre (170, 30).
        let straight = 100 - 30.0, arc = Double.pi * 30 / 2
        let length = 2 * (200 - 60) + 2 * (240 - 60) + 2 * Double.pi * 30
        let p = Track.point(at: (straight + arc / 2) / length, in: rect, radius: 30)
        XCTAssertEqual(hypot(p.x - 170, p.y - 30), 30, accuracy: 0.5)
    }
}

final class CrownTests: XCTestCase {
    func testAnyCrownValueMapsToAWorktree() {
        XCTAssertEqual(Dial.index(forCrown: 5, count: 4), 1)
        XCTAssertEqual(Dial.index(forCrown: -1, count: 4), 3)
        XCTAssertEqual(Dial.index(forCrown: 7.4, count: 4), 3)
        XCTAssertEqual(Dial.index(forCrown: 3, count: 0), 0)
    }

    func testTheCrownMovesToAnIndexTheShortWay() {
        XCTAssertEqual(Dial.crownValue(for: 1, near: 8, count: 4), 9)
        XCTAssertEqual(Dial.crownValue(for: 3, near: 8, count: 4), 7)
        XCTAssertEqual(Dial.crownValue(for: 0, near: 0, count: 4), 0)
        XCTAssertEqual(Dial.crownValue(for: 2, near: -3, count: 5), -3, "-3 is already index 2")
    }
}

final class UnevenTrackTests: XCTestCase {
    func testBottomCornersCanFollowTheDisplayWithALargerRadius() {
        let rect = CGRect(x: 0, y: 0, width: 200, height: 240)
        // Still symmetric: quarter points at the side middles, half at bottom centre.
        let half = Track.point(at: 0.5, in: rect, top: 20, bottom: 50)
        XCTAssertEqual(half.x, 100, accuracy: 0.5)
        XCTAssertEqual(half.y, 240, accuracy: 0.5)
        // Every point near the bottom-right corner lies on its arc: 50 from the centre (150, 190).
        for t in stride(from: 0.30, through: 0.42, by: 0.01) {
            let p = Track.point(at: t, in: rect, top: 20, bottom: 50)
            if p.x > 150 && p.y > 190 { XCTAssertEqual(hypot(p.x - 150, p.y - 190), 50, accuracy: 0.5) }
        }
        // The top-right corner keeps its own radius: 20 from (180, 20).
        for t in stride(from: 0.10, through: 0.20, by: 0.01) {
            let p = Track.point(at: t, in: rect, top: 20, bottom: 50)
            if p.x > 180 && p.y < 20 { XCTAssertEqual(hypot(p.x - 180, p.y - 20), 20, accuracy: 0.5) }
        }
    }

    func testTheSlideStartsAtZeroAndSettlesAtExactlyOne() {
        XCTAssertEqual(Dial.slide(after: -1), 0)
        XCTAssertEqual(Dial.slide(after: 0), 0)
        XCTAssertEqual(Dial.slide(after: Dial.slideSettle), 1)
        XCTAssertEqual(Dial.slide(after: 5), 1)
    }

    func testTheSlideRisesFastWithASmallOvershoot() {
        XCTAssertGreaterThan(Dial.slide(after: 0.12), 0.5, "most of the way within 120 ms")
        let samples = stride(from: 0.0, through: Dial.slideSettle, by: 0.005).map { Dial.slide(after: $0) }
        XCTAssertLessThan(samples.max() ?? 0, 1.04, "the overshoot stays small")
        XCTAssertEqual(samples.last ?? 0, 1, accuracy: 0.002, "no jump when it settles")
    }
}
