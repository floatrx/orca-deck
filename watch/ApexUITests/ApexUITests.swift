import XCTest

/// Acceptance tests of .work/plans/apex-v2.md on the watchOS simulator, against the running bridge
/// (`watch/configure.sh http://localhost:7353` before the run). Environment (xcodebuild `TEST_RUNNER_` prefix):
/// - `APEX_SCRATCH_SESSION` — a scratch chat session with a long last message and a pending Next: the crown
///   scrolls its message, and the Undo test cancels its send inside the 3 s window;
/// - `APEX_TAP_SESSION` — the session the tap test reveals in Orca. Leave it unset unless that pane is already
///   on the Mac's screen: the tap switches Orca's active worktree.
/// No other session is ever targeted. A launch argument (`-apex.layout cards`) picks a setting for one run.
final class ApexUITests: XCTestCase {
    private let environment = ProcessInfo.processInfo.environment

    override func setUp() {
        continueAfterFailure = false
    }

    private func launch(_ arguments: [String] = []) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = arguments
        app.launch()
        return app
    }

    private func session(_ name: String) throws -> String {
        guard let value = environment[name], !value.isEmpty else { throw XCTSkip(name) }
        return value
    }

    /// The knob centre's accessibility value "4/9" → (index 3, count 9).
    private func position(_ centre: XCUIElement) -> (index: Int, count: Int)? {
        guard let value = centre.value as? String else { return nil }
        let parts = value.split(separator: "/").compactMap { Int($0) }
        guard parts.count == 2, parts[1] > 0 else { return nil }
        return (parts[0] - 1, parts[1])
    }

    private func waitForWorktrees(_ centre: XCUIElement) -> (index: Int, count: Int) {
        XCTAssertTrue(centre.waitForExistence(timeout: 20), "the knob centre")
        let deadline = Date().addingTimeInterval(20)
        while Date() < deadline {
            if let position = position(centre) { return position }
            Thread.sleep(forTimeInterval: 0.3)
        }
        XCTFail("no worktrees from the bridge")
        return (0, 0)
    }

    private func cards(_ app: XCUIApplication) -> XCUIElementQuery {
        app.buttons.matching(identifier: "worktree-card")
    }

    private func waitForCards(_ app: XCUIApplication) -> Int {
        XCTAssertTrue(cards(app).firstMatch.waitForExistence(timeout: 20), "the cards")
        Thread.sleep(forTimeInterval: 1)
        return cards(app).count
    }

    /// Acceptance: the crown moves one worktree per detent and wraps past the last one. The simulator ignores
    /// tiny turns; 0.22 of a rotation is one detent there (probed: 0.2–0.3 → one, 0.4 → two).
    func testCrownMovesExactlyOneWorktreePerDetentAndWraps() {
        let app = launch(["-apexSelectRepo", "orca-deck", "-apex.hideInactive", "NO"])
        let centre = app.buttons["knob-centre"]
        var (index, count) = waitForWorktrees(centre)
        XCTAssertGreaterThan(count, 2)
        for _ in 0...count {
            XCUIDevice.shared.rotateDigitalCrown(delta: 0.22, velocity: 1)
            Thread.sleep(forTimeInterval: 0.8)
            let next = position(centre)?.index
            XCTAssertEqual(next, (index + 1) % count, "one detent must move exactly one worktree, wrapping round")
            index = next ?? index
        }
    }

    /// Acceptance: swipe left opens the agent page; swipe right from the main page opens Settings.
    func testSwipeLeftOpensTheAgentAndSwipeRightSettings() {
        let app = launch(["-apexSelectRepo", "orca-deck"])
        let centre = app.buttons["knob-centre"]
        _ = waitForWorktrees(centre)
        app.swipeLeft()
        XCTAssertTrue(app.descendants(matching: .any)["agent-page"].waitForExistence(timeout: 5), "agent page")
        app.swipeRight()
        XCTAssertTrue(centre.waitForExistence(timeout: 5))
        Thread.sleep(forTimeInterval: 0.5)
        XCTAssertTrue(centre.isHittable, "back on the knob")
        app.swipeRight()
        XCTAssertTrue(app.staticTexts["settings-title"].waitForExistence(timeout: 5), "settings page")
        app.swipeLeft()
        Thread.sleep(forTimeInterval: 0.5)
        XCTAssertTrue(centre.isHittable, "back on the knob")
    }

    /// Acceptance: on the agent page the crown scrolls the message, and the knob keeps its selection.
    func testTheCrownScrollsTheMessageOnTheAgentPage() throws {
        let session = try session("APEX_SCRATCH_SESSION")
        let app = launch(["-apexSelectRepo", "orca-deck", "-apexPickAgent", session])
        let centre = app.buttons["knob-centre"]
        let start = waitForWorktrees(centre)
        app.swipeLeft()
        let message = app.descendants(matching: .any)["agent-message"]
        XCTAssertTrue(message.waitForExistence(timeout: 10))
        Thread.sleep(forTimeInterval: 4)  // the full message replaces Orca's 200-character cut
        let before = message.frame.minY
        for _ in 0..<4 {
            XCUIDevice.shared.rotateDigitalCrown(delta: 0.3, velocity: 1)
            Thread.sleep(forTimeInterval: 0.5)
        }
        XCTAssertLessThan(message.frame.minY, before, "the crown must scroll the message on the agent page")
        app.swipeRight()
        XCTAssertTrue(centre.waitForExistence(timeout: 5))
        XCTAssertEqual(position(centre)?.index, start.index, "the knob must not turn while the agent page has the crown")
    }

    /// Acceptance: a tap on the centre sends `go` (the runner reads the timing from the bridge trail).
    func testTapOnTheCentreFocusesTheChosenWorktree() throws {
        let session = try session("APEX_TAP_SESSION")
        let app = launch(["-apexSelectRepo", "orca-deck", "-apexPickAgent", session])
        let centre = app.buttons["knob-centre"]
        _ = waitForWorktrees(centre)
        centre.tap()
        Thread.sleep(forTimeInterval: 2)
        XCTAssertTrue(centre.exists)
    }

    /// Acceptance: Undo within 3 s sends nothing (the runner checks the trail has no `say` for the session).
    func testUndoWithinThreeSecondsSendsNothing() throws {
        let session = try session("APEX_SCRATCH_SESSION")
        let app = launch(["-apexPage", "1", "-apexSelectRepo", "orca-deck", "-apexPickAgent", session])
        let send = app.buttons["send-button"]
        XCTAssertTrue(send.waitForExistence(timeout: 20), "Send on the scratch session's Next")
        send.tap()
        let undo = app.buttons["undo-button"]
        XCTAssertTrue(undo.waitForExistence(timeout: 3), "the undo countdown")
        undo.tap()
        XCTAssertTrue(send.waitForExistence(timeout: 3), "Undo returns the card to Send")
        Thread.sleep(forTimeInterval: 5)  // well past the 3 s window
        XCTAssertTrue(send.exists, "still nothing sent")
    }

    /// Feedback 0.3: the cards layout lists the worktrees, the chosen one marked, and the crown scrolls it.
    func testTheCardsLayoutListsTheWorktreesAndScrollsWithTheCrown() {
        let app = launch(["-apexSelectRepo", "orca-deck", "-apex.layout", "cards", "-apex.hideInactive", "NO"])
        XCTAssertGreaterThan(waitForCards(app), 3)
        XCTAssertEqual(cards(app).matching(NSPredicate(format: "value == 'selected'")).count, 1, "one chosen card")
        let first = cards(app).element(boundBy: 0)
        let before = first.frame.minY
        for _ in 0..<3 {
            XCUIDevice.shared.rotateDigitalCrown(delta: 0.3, velocity: 1)
            Thread.sleep(forTimeInterval: 0.5)
        }
        XCTAssertNotEqual(first.frame.minY, before, "the crown must scroll the cards")
        app.swipeLeft()
        XCTAssertTrue(app.descendants(matching: .any)["agent-page"].waitForExistence(timeout: 5), "agent page")
    }

    /// Feedback 0.3: "Hide inactive" (on by default) drops the worktrees without agents.
    func testHideInactiveDropsTheWorktreesWithoutAgents() {
        var app = launch(["-apex.layout", "cards", "-apex.hideInactive", "NO"])
        let all = waitForCards(app)
        app.terminate()
        app = launch(["-apex.layout", "cards"])
        let shown = waitForCards(app)
        XCTAssertLessThan(shown, all, "the default hides the worktrees without agents")
    }

    /// Feedback 0.3: Settings (swipe right) switches the main page between the knob and the cards.
    func testSettingsSwitchesTheLayout() {
        let app = launch(["-apexSelectRepo", "orca-deck"])
        XCTAssertTrue(app.buttons["knob-centre"].waitForExistence(timeout: 20), "starts on the knob")
        app.swipeRight()
        XCTAssertTrue(app.staticTexts["settings-title"].waitForExistence(timeout: 5))
        app.buttons["Cards"].tap()
        app.swipeLeft()
        XCTAssertTrue(cards(app).firstMatch.waitForExistence(timeout: 5), "the cards replace the knob")
        app.swipeRight()
        app.buttons["Knob"].tap()
        app.swipeLeft()
        XCTAssertTrue(app.buttons["knob-centre"].waitForExistence(timeout: 5), "back to the knob")
    }

    /// Questions: Undo within 3 s answers nothing (the runner checks the trail has no `answer`). Needs the scratch
    /// session to have a pending question. Named so it runs before the test that answers.
    func testAnswerUndoWithinThreeSecondsAnswersNothing() throws {
        let session = try session("APEX_QUESTION_SESSION")
        let app = launch(["-apexPage", "1", "-apexSelectRepo", "orca-deck", "-apexPickAgent", session])
        let options = app.buttons.matching(identifier: "answer-option")
        XCTAssertTrue(options.firstMatch.waitForExistence(timeout: 20), "the question's options")
        XCTAssertFalse(app.buttons["send-button"].exists, "no Send while a question is pending")
        options.element(boundBy: 0).tap()
        let undo = app.buttons["undo-button"]
        XCTAssertTrue(undo.waitForExistence(timeout: 3), "the answer countdown")
        undo.tap()
        XCTAssertTrue(options.firstMatch.waitForExistence(timeout: 3), "Undo brings the options back")
        Thread.sleep(forTimeInterval: 5)  // well past the 3 s window
        XCTAssertTrue(options.firstMatch.exists, "still not answered")
    }

    /// Questions: a tap on an option answers the scratch session's question once the countdown runs out.
    func testAnswerWithAnOptionTap() throws {
        let session = try session("APEX_QUESTION_SESSION")
        let app = launch(["-apexPage", "1", "-apexSelectRepo", "orca-deck", "-apexPickAgent", session])
        let options = app.buttons.matching(identifier: "answer-option")
        XCTAssertTrue(options.firstMatch.waitForExistence(timeout: 20), "the question's options")
        options.element(boundBy: 1).tap()
        let question = app.descendants(matching: .any)["agent-question"]
        let gone = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: question)
        XCTAssertEqual(XCTWaiter.wait(for: [gone], timeout: 25), .completed, "the question is answered and leaves the page")
    }
}
