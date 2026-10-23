// swift-tools-version:5.9
// ApexCore: the platform-free logic of the Apex watch app — protocol v1 models (../../protocol.md),
// the event cursor, the beacon pattern and display formatting. Tested on macOS with `swift test`.
import PackageDescription

let package = Package(
    name: "ApexCore",
    platforms: [.macOS(.v13), .watchOS(.v10)],
    products: [
        .library(name: "ApexCore", targets: ["ApexCore"]),
    ],
    targets: [
        .target(name: "ApexCore"),
        .testTarget(name: "ApexCoreTests", dependencies: ["ApexCore"], resources: [.copy("Fixtures")]),
    ]
)
