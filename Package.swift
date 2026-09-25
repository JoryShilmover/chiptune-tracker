// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ChiptuneTracker",
    platforms: [.iOS(.v18), .macOS(.v15)],
    products: [
        .library(name: "DSSPU", targets: ["DSSPU"]),
        .executable(name: "spu-render", targets: ["spu-render"]),
    ],
    targets: [
        // Nintendo DS sound hardware (SPU) emulator. Portable C++, no dependencies.
        .target(name: "DSSPU", path: "Core/SPU"),

        // Command-line tool that renders demo songs to WAV.
        .executableTarget(name: "spu-render", dependencies: ["DSSPU"], path: "Tools/spu-render"),

        // Unit tests. Run with `swift run spu-tests` (or scripts/test.sh).
        .executableTarget(name: "spu-tests", dependencies: ["DSSPU"], path: "Tests/SPUTests"),
    ],
    cxxLanguageStandard: .cxx20
)
