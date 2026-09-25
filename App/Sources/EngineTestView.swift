import SwiftUI
import TrackerAudio

/// Milestone 1 test screen: plays the hardcoded demo song and shows what the
/// engine is doing (position, channel activity, levels, latency and render load).
struct EngineTestView: View {
    let playback: PlaybackController

    private var status: EngineStatus { playback.status }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                transport
                StepGrid(step: status.step, playing: status.playing)
                ChannelStrip(levels: status.channelLevels)
                StereoMeter(left: status.peakLeft, right: status.peakRight)
                speedControl
                diagnostics
                if let error = playback.lastError {
                    Text(error).foregroundStyle(.red).font(.callout)
                }
            }
            .padding(20)
            .frame(maxWidth: 640)
            .frame(maxWidth: .infinity)
        }
        .background(Color.black)
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Chiptune Tracker")
                .font(.system(.largeTitle, design: .monospaced, weight: .bold))
            Text("Engine test · DS sound hardware at \(Int(playback.latency.engineSampleRate.rounded())) Hz")
                .font(.system(.subheadline, design: .monospaced))
                .foregroundStyle(.secondary)
        }
    }

    private var transport: some View {
        Button {
            playback.togglePlayback()
        } label: {
            Label(status.playing ? "Stop" : "Play demo", systemImage: status.playing ? "stop.fill" : "play.fill")
                .font(.system(.title3, design: .monospaced, weight: .semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
        }
        .buttonStyle(.borderedProminent)
        .tint(status.playing ? .red : .green)
        .accessibilityIdentifier("transport")
    }

    private var speedControl: some View {
        @Bindable var playback = playback
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Tick rate")
                Spacer()
                Text("\(Int(playback.tickRate)) Hz")
                    .monospacedDigit()
                Button("Reset") { playback.tickRate = 60 }
                    .disabled(playback.tickRate == 60)
            }
            .font(.system(.callout, design: .monospaced))
            Slider(value: $playback.tickRate, in: 30...120, step: 1)
        }
    }

    private var diagnostics: some View {
        let latency = playback.latency
        return VStack(alignment: .leading, spacing: 6) {
            Text("Diagnostics").font(.system(.headline, design: .monospaced))
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                row("Audio", playback.isAudioRunning ? "running" : "stopped")
                row("Hardware rate", "\(Int(latency.hardwareSampleRate)) Hz")
                row("I/O buffer", milliseconds(latency.ioBufferSeconds))
                row("Output latency", milliseconds(latency.outputLatencySeconds))
                row("Total latency", milliseconds(latency.totalSeconds))
                row("Callback", "\(status.lastFrameCount) frames · \(micros(status.callbackBudgetMicros)) budget")
                row("Render time", "\(micros(status.lastRenderMicros)) · worst \(micros(status.maxRenderMicros))")
                row("Worst-case load", String(format: "%.1f%%", status.worstCaseLoad * 100))
                row("Callbacks", "\(status.renderCalls)")
            }
            .font(.system(.footnote, design: .monospaced))
            Button("Reset worst case") { playback.resetStats() }
                .font(.system(.footnote, design: .monospaced))
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(value).monospacedDigit()
        }
    }

    private func milliseconds(_ seconds: Double) -> String { String(format: "%.1f ms", seconds * 1000) }
    private func micros(_ us: Double) -> String { String(format: "%.0f µs", us) }
}

/// 32 steps of the demo loop; the current step is lit.
private struct StepGrid: View {
    let step: UInt32
    let playing: Bool

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 4), count: 16)

    var body: some View {
        LazyVGrid(columns: columns, spacing: 4) {
            ForEach(0..<32, id: \.self) { index in
                RoundedRectangle(cornerRadius: 3)
                    .fill(color(for: index))
                    .aspectRatio(1, contentMode: .fit)
            }
        }
        .accessibilityLabel(playing ? "Step \(step % 32 + 1) of 32" : "Stopped")
    }

    private func color(for index: Int) -> Color {
        if playing && index == Int(step % 32) { return .green }
        return index % 4 == 0 ? Color.white.opacity(0.25) : Color.white.opacity(0.12)
    }
}

/// One level bar per hardware channel, colored by what the channel can play.
private struct ChannelStrip: View {
    let levels: [Float]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .bottom, spacing: 4) {
                ForEach(levels.indices, id: \.self) { channel in
                    VStack(spacing: 4) {
                        GeometryReader { proxy in
                            let height = proxy.size.height * CGFloat(min(max(levels[channel] * 2, 0), 1))
                            VStack {
                                Spacer(minLength: 0)
                                RoundedRectangle(cornerRadius: 2)
                                    .fill(kind(channel).color)
                                    .frame(height: max(height, 2))
                            }
                        }
                        .frame(height: 64)
                        Text("\(channel)")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                }
            }
            HStack(spacing: 14) {
                ForEach([ChannelKind.sample, .psg, .noise], id: \.self) { kind in
                    Label(kind.label, systemImage: "circle.fill")
                        .foregroundStyle(kind.color)
                }
            }
            .font(.system(.caption2, design: .monospaced))
        }
    }

    private func kind(_ channel: Int) -> ChannelKind {
        switch channel {
        case 14...15: .noise
        case 8...13: .psg
        default: .sample
        }
    }
}

private enum ChannelKind: Hashable {
    case sample, psg, noise

    var label: String {
        switch self {
        case .sample: "Sample"
        case .psg: "PSG"
        case .noise: "Noise"
        }
    }

    var color: Color {
        switch self {
        case .sample: .cyan
        case .psg: .orange
        case .noise: .pink
        }
    }
}

private struct StereoMeter: View {
    let left: Float
    let right: Float

    var body: some View {
        VStack(spacing: 6) {
            bar("L", left)
            bar("R", right)
        }
        .font(.system(.caption, design: .monospaced))
    }

    private func bar(_ label: String, _ level: Float) -> some View {
        HStack {
            Text(label).foregroundStyle(.secondary)
            GeometryReader { proxy in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.white.opacity(0.1))
                    Capsule()
                        .fill(level > 0.95 ? Color.red : Color.green)
                        .frame(width: proxy.size.width * CGFloat(min(level, 1)))
                }
            }
            .frame(height: 8)
        }
    }
}

#Preview {
    EngineTestView(playback: PlaybackController())
        .preferredColorScheme(.dark)
}
