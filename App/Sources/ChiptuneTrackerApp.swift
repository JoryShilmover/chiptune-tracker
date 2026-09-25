import SwiftUI
import TrackerAudio

@main
struct ChiptuneTrackerApp: App {
    @State private var playback = PlaybackController()

    var body: some Scene {
        WindowGroup {
            EngineTestView(playback: playback)
                .preferredColorScheme(.dark)
                .task {
                    // `-autoplay` starts playback on launch, for testing.
                    if ProcessInfo.processInfo.arguments.contains("-autoplay") {
                        playback.play()
                    }
                }
        }
    }
}
