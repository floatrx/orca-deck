import XCTest
@testable import ApexCore

final class RoutesTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_000_000)

    func testOnTheFirstRouteTheRestFollowByPriority() {
        let plan = Routes.order(current: 0, count: 3, now: now, lastProbe: nil)
        XCTAssertEqual(plan.order, [0, 1, 2])
        XCTAssertFalse(plan.probe)
    }

    func testOffTheFirstRouteTheWorkingOneGoesFirst() {
        let plan = Routes.order(current: 2, count: 3, now: now, lastProbe: now.addingTimeInterval(-10))
        XCTAssertEqual(plan.order, [2, 0, 1])
        XCTAssertFalse(plan.probe)
    }

    func testOffTheFirstRouteTheFirstIsProbedOncePerInterval() {
        let plan = Routes.order(current: 2, count: 3, now: now, lastProbe: now.addingTimeInterval(-Routes.probeInterval))
        XCTAssertEqual(plan.order, [0, 2, 1])
        XCTAssertTrue(plan.probe)
        XCTAssertTrue(Routes.order(current: 1, count: 2, now: now, lastProbe: nil).probe, "never probed yet")
    }

    func testAnOutOfRangeCurrentIsClampedAndNoRoutesGiveNoOrder() {
        XCTAssertEqual(Routes.order(current: 7, count: 2, now: now, lastProbe: now).order, [1, 0])
        XCTAssertEqual(Routes.order(current: 0, count: 0, now: now, lastProbe: nil).order, [])
    }

    func testNetworkFailuresSwitchRoutesButBridgeAnswersDoNot() {
        XCTAssertTrue(Routes.isRouteFailure(URLError(.timedOut)))
        XCTAssertTrue(Routes.isRouteFailure(URLError(.cannotFindHost)))
        XCTAssertTrue(Routes.isRouteFailure(URLError(.cannotConnectToHost)))
        XCTAssertFalse(Routes.isRouteFailure(URLError(.badServerResponse)))
        XCTAssertFalse(Routes.isRouteFailure(NSError(domain: "bridge", code: 401)))
    }

    func testLocalRoutesAreTheLanOnes() throws {
        for local in ["http://floatrxs-MacBook-Pro-M4.local:7353", "http://192.168.2.109:7353", "http://10.0.0.5:7353",
                      "http://172.20.1.1:7353", "http://localhost:7353"] {
            XCTAssertTrue(Routes.isLocal(try XCTUnwrap(URL(string: local))), local)
        }
        let tunnel = try XCTUnwrap(URL(string: "https://apex.example.com"))
        XCTAssertFalse(Routes.isLocal(tunnel))
        XCTAssertEqual(Routes.label(tunnel), "tunnel")
        XCTAssertEqual(Routes.timeout(for: tunnel), 6)
        XCTAssertEqual(Routes.timeout(for: try XCTUnwrap(URL(string: "http://192.168.2.109:7353"))), 2.5)
    }

    func testTheLearnedLanGoesFirstWithoutDuplicates() throws {
        let learned = try XCTUnwrap(URL(string: "http://192.168.2.109:7353"))
        let configured = ["http://192.168.31.246:7353", "http://192.168.2.109:7353/", "https://apex.example.com"]
            .compactMap(URL.init(string:))
        XCTAssertEqual(Routes.merge(learned: learned, configured: configured).map(\.absoluteString),
                       ["http://192.168.2.109:7353", "http://192.168.31.246:7353", "https://apex.example.com"])
        XCTAssertEqual(Routes.merge(learned: nil, configured: configured).count, 3)
    }

    func testOnlyAnHttpLanUrlIsLearnable() {
        XCTAssertEqual(Routes.learnable(["http://10.0.0.5:7353", "http://192.168.1.2:7353"])?.absoluteString, "http://10.0.0.5:7353")
        XCTAssertNil(Routes.learnable(["https://apex.example.com", "not a url"]))
        XCTAssertNil(Routes.learnable(nil))
    }

    func testTheStateCarriesTheLanUrls() throws {
        let json = Data(#"{"v":1,"seq":5,"worktrees":[],"events":[],"lan":["http://192.168.2.109:7353"]}"#.utf8)
        XCTAssertEqual(try StateResponse.decode(json).lan, ["http://192.168.2.109:7353"])
        let old = Data(#"{"v":1,"seq":5,"worktrees":[],"events":[]}"#.utf8)
        XCTAssertNil(try StateResponse.decode(old).lan, "an older bridge has no lan field")
    }

    func testGatewayStatusesMeanTheTunnelCannotReachTheBridge() {
        for code in [502, 503, 504, 521, 522, 530] { XCTAssertTrue(Routes.isGatewayStatus(code), "\(code)") }
        for code in [200, 400, 401, 404, 500] { XCTAssertFalse(Routes.isGatewayStatus(code), "\(code)") }
    }
}
