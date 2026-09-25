// realtime-check: plays the demo in real time through AVAudioEngine on a Mac
// and reports render timing, to check the real-time path without a device.
//
//   swift run -c release realtime-check [--seconds N] [--mute]

import Foundation
import TrackerAudio

var seconds = 5.0
var muted = false
var arguments = CommandLine.arguments.dropFirst().makeIterator()
while let argument = arguments.next() {
    switch argument {
    case "--seconds": seconds = arguments.next().flatMap(Double.init) ?? seconds
    case "--mute": muted = true
    default:
        print("usage: realtime-check [--seconds N] [--mute]")
        exit(2)
    }
}

@MainActor
func run() async -> Int32 {
    let controller = PlaybackController()
    controller.outputVolume = muted ? 0 : 1
    controller.play()
    guard controller.isAudioRunning else {
        print("error: \(controller.lastError ?? "audio didn't start")")
        return 1
    }

    try? await Task.sleep(for: .seconds(seconds))
    let status = controller.status
    let latency = controller.latency
    controller.stop()

    let expectedFrames = seconds * latency.engineSampleRate
    print(String(format: "played %.1f s%@", seconds, muted ? " (muted)" : ""))
    print(String(format: "engine rate %.1f Hz -> hardware %.0f Hz", latency.engineSampleRate, latency.hardwareSampleRate))
    print(String(format: "output latency %.1f ms", latency.outputLatencySeconds * 1000))
    print("render calls \(status.renderCalls), frames \(status.framesRendered) (expected ~\(Int(expectedFrames)))")
    print(String(format: "callback %u frames = %.0f us budget; render last %.0f us, worst %.0f us (%.1f%% load)",
                 status.lastFrameCount, status.callbackBudgetMicros, status.lastRenderMicros,
                 status.maxRenderMicros, status.worstCaseLoad * 100))
    print("steps played \(status.step)")

    // The audio thread must keep up: frames delivered should track wall time.
    let delivered = Double(status.framesRendered) / expectedFrames
    guard delivered > 0.95, status.worstCaseLoad < 0.5 else {
        print(String(format: "FAIL: delivered %.0f%% of expected frames, worst-case load %.0f%%",
                     delivered * 100, status.worstCaseLoad * 100))
        return 1
    }
    print("ok")
    return 0
}

let code = await run()
exit(code)
