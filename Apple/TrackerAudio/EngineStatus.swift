import TrackerEngineC

/// A snapshot of the engine, polled from the UI.
public struct EngineStatus: Sendable, Equatable {
    public static let channelCount = Int(TE_CHANNEL_COUNT)

    public var playing = false
    public var tick: UInt32 = 0
    public var step: UInt32 = 0
    public var tickRate: Double = 60
    /// 0-1, the loudest sample since the previous poll.
    public var peakLeft: Float = 0
    public var peakRight: Float = 0
    /// 0-1 per hardware channel, the loudest output since the previous poll.
    public var channelLevels = [Float](repeating: 0, count: EngineStatus.channelCount)
    public var renderCalls: UInt64 = 0
    public var framesRendered: UInt64 = 0
    /// Frames the audio system asked for in the latest callback.
    public var lastFrameCount: UInt32 = 0
    public var lastRenderMicros: Double = 0
    public var maxRenderMicros: Double = 0

    public init() {}

    init(_ raw: TEStatus) {
        playing = raw.playing
        tick = raw.tick
        step = raw.step
        tickRate = raw.tick_rate
        peakLeft = raw.peak_left
        peakRight = raw.peak_right
        channelLevels = withUnsafeBytes(of: raw.channel_levels) { Array($0.bindMemory(to: Float.self)) }
        renderCalls = raw.render_calls
        framesRendered = raw.frames_rendered
        lastFrameCount = raw.last_frame_count
        lastRenderMicros = raw.last_render_us
        maxRenderMicros = raw.max_render_us
    }

    /// How long the latest callback's audio lasts, at the engine's rate.
    public var callbackBudgetMicros: Double {
        Double(lastFrameCount) / te_output_sample_rate() * 1_000_000
    }

    /// Fraction of the callback's time budget spent rendering (worst case).
    public var worstCaseLoad: Double {
        callbackBudgetMicros > 0 ? maxRenderMicros / callbackBudgetMicros : 0
    }
}

/// Output latency as reported by the audio system.
public struct LatencyReport: Sendable, Equatable {
    public var engineSampleRate: Double = 0
    public var hardwareSampleRate: Double = 0
    /// The I/O buffer the system renders in (iOS only).
    public var ioBufferSeconds: Double = 0
    /// Delay from the output buffer to the speaker or headphones.
    public var outputLatencySeconds: Double = 0

    public init() {}

    /// From a change in the engine to hearing it: at most one I/O buffer plus
    /// the output path.
    public var totalSeconds: Double { ioBufferSeconds + outputLatencySeconds }
}
