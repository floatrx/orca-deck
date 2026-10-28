import XCTest

/// Acceptance tests of .work/plans/apex-v2.md on the watchOS simulator, against the running bridge
/// (`watch/configure.sh http://localhost:7353` before the run). Environment (xcodebuild `TEST_RUNNER_` prefix):
/// - `APEX_SCRATCH_SESSION` — a scratch chat session with a long last message and a pending Next: the crown
///   scrolls its message (Send goes at once since 2026-10-28, so no test presses it);
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

    /// The number of worktrees the cards list holds — its accessibility value, because a LazyVStack only builds
    /// the cards on screen and a query would count those.
    private func waitForCards(_ app: XCUIApplication) -> Int {
        XCTAssertTrue(cards(app).firstMatch.waitForExistence(timeout: 20), "the cards")
        Thread.sleep(forTimeInterval: 1)
        let list = app.descendants(matching: .any)["cards-list"]
        return Int(list.value as? String ?? "") ?? cards(app).count
    }

    /// Acceptance: the crown moves one worktree per detent and wraps past the last one. The simulator turns a
    /// rotation into a varying number of detents (0.22 of a turn is one or two), so each step compares the move of
    /// the selection with the detents the crown really produced (`knob-crown`), and the run goes past the end.
    func testCrownMovesExactlyOneWorktreePerDetentAndWraps() {
        let app = launch(["-apexSelectRepo", "orca-deck", "-apex.hideInactive", "NO", "-apex.hideCompletedAfter", "0"])
        let centre = app.buttons["knob-centre"]
        let crown = app.descendants(matching: .any)["knob-crown"]
        var (index, count) = waitForWorktrees(centre)
        XCTAssertGreaterThan(count, 2)
        var detents = Int(crown.value as? String ?? "") ?? 0
        var travelled = 0
        var turns = 0
        var wrapped = false
        // A turn of the same size gives 0, 1 or 2 detents in the simulator: the rule is "the selection moves by
        // exactly the detents the crown produced", and the run keeps turning until it has gone past the end.
        while travelled <= count, turns < count * 4 {
            turns += 1
            XCUIDevice.shared.rotateDigitalCrown(delta: 0.22, velocity: 1)
            Thread.sleep(forTimeInterval: 0.8)
            let nowDetents = Int(crown.value as? String ?? "") ?? detents
            let step = nowDetents - detents
            XCTAssertGreaterThanOrEqual(step, 0, "a forward turn never goes back")
            let next = position(centre)?.index ?? index
            XCTAssertEqual(next, (index + step) % count,
                           "\(step) detent(s) must move exactly \(step) worktree(s), wrapping round")
            if next < index { wrapped = true }
            index = next
            detents = nowDetents
            travelled += step
        }
        XCTAssertGreaterThan(travelled, count, "the crown went all the way round")
        XCTAssertTrue(wrapped, "the selection wraps past the last worktree")
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

    /// After a send the main page comes back (user, 2026-10-28); `-apexDemo home` returns without sending.
    func testTheMainPageComesBackAfterASend() {
        let app = launch(["-apexSelectRepo", "orca-deck", "-apexDemo", "home"])
        let centre = app.buttons["knob-centre"]
        _ = waitForWorktrees(centre)
        app.swipeLeft()
        XCTAssertTrue(app.descendants(matching: .any)["agent-page"].waitForExistence(timeout: 5), "agent page")
        Thread.sleep(forTimeInterval: 3.5)
        XCTAssertTrue(centre.isHittable, "back on the knob")
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

    /// Feedback 0.3: the cards layout lists the worktrees, the chosen one marked, and the crown scrolls it.
    func testTheCardsLayoutListsTheWorktreesAndScrollsWithTheCrown() {
        let app = launch(["-apexSelectRepo", "orca-deck", "-apex.layout", "cards", "-apex.hideInactive", "NO",
                          "-apex.hideCompletedAfter", "0"])
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
        var app = launch(["-apex.layout", "cards", "-apex.hideInactive", "NO", "-apex.hideCompletedAfter", "0"])
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

    /// Questions: a tap on an option answers the scratch session's question at once.
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
