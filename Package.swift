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

        // Real-time engine: transport, song playback and telemetry around the SPU.
        .target(name: "TrackerCore", dependencies: ["DSSPU"], path: "Core/Engine"),

        // C interface to TrackerCore, so Swift can use it without C++ interop.
        .target(name: "TrackerEngineC", dependencies: ["TrackerCore"], path: "Core/EngineC"),

        // Command-line tool that renders the demo song to WAV.
        .executableTarget(name: "spu-render", dependencies: ["TrackerCore"], path: "Tools/spu-render"),

        // C++ unit tests. Run with `swift run core-tests` (or scripts/test.sh).
        .executableTarget(name: "core-tests", dependencies: ["TrackerCore"], path: "Tests/CoreTests"),
    ],
    cxxLanguageStandard: .cxx20
)
