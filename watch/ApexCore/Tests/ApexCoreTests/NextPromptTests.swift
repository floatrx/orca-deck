import XCTest
@testable import ApexCore

final class NextPromptTests: XCTestCase {
    private let variants = ["commit and push", "run e2e", "switch task"]

    func testNothingTickedSendsNothing() {
        XCTAssertNil(NextPrompt.compose(variants, picked: []))
    }

    func testOneTickedVariantGoesVerbatim() {
        XCTAssertEqual(NextPrompt.compose(variants, picked: [1]), "run e2e")
    }

    func testSeveralGoAsANumberedListInTheAgentsOrder() {
        XCTAssertEqual(NextPrompt.compose(variants, picked: [2, 0]), "1. commit and push\n2. switch task")
    }

    func testIndicesOutsideTheListAreIgnored() {
        XCTAssertEqual(NextPrompt.compose(variants, picked: [0, 7]), "commit and push")
    }
}
