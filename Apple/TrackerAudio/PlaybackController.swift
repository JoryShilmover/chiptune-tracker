import AVFoundation
import Observation
import TrackerEngineC

/// Plays the tracker engine through AVAudioEngine and publishes its telemetry
/// for the UI.
///
/// The engine renders at the DS's native ~32,728.5 Hz. An `AVAudioSourceNode`
/// pulls from it on the real-time audio thread, and AVAudioEngine's mixer
/// resamples to the hardware rate. The render callback only calls into C and
/// touches no Swift objects, so it doesn't allocate or take locks.
@MainActor
@Observable
public final class PlaybackController {
    public private(set) var status = EngineStatus()
    public private(set) var latency = LatencyReport()
    public private(set) var isAudioRunning = false
    public private(set) var lastError: String?

    /// The engine's tick rate in Hz. The demo song plays at 60.
    public var tickRate: Double = 60 {
        didSet { te_engine_set_tick_rate(engine.pointer, tickRate) }
    }

    /// Output volume, 0-1. Rendering continues at 0, which tools use to test
    /// the real-time path silently.
    public var outputVolume: Float = 1 {
        didSet { audioEngine.mainMixerNode.outputVolume = outputVolume }
    }

    @ObservationIgnored private let engine: EngineHandle
    @ObservationIgnored private let audioEngine = AVAudioEngine()
    @ObservationIgnored private var sourceNode: AVAudioSourceNode?
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    public init() {
        engine = EngineHandle(pointer: te_engine_create_demo())
        observeSystemEvents()
    }

    isolated deinit {
        pollTask?.cancel()
        observers.forEach(NotificationCenter.default.removeObserver)
        audioEngine.stop()
        te_engine_destroy(engine.pointer)
    }

    // MARK: - Transport

    public func play() {
        startAudioIfNeeded()
        te_engine_play(engine.pointer)
    }

    public func stop() {
        te_engine_stop(engine.pointer)
    }

    public func togglePlayback() {
        status.playing ? stop() : play()
    }

    public func resetStats() {
        te_engine_reset_stats(engine.pointer)
    }

    // MARK: - Audio setup

    private func startAudioIfNeeded() {
        guard !isAudioRunning else { return }
        do {
            try configureSession()
            if sourceNode == nil { attachSourceNode() }
            audioEngine.prepare()
            try audioEngine.start()
            isAudioRunning = true
            lastError = nil
            updateLatency()
            startPolling()
        } catch {
            lastError = "Couldn't start audio: \(error.localizedDescription)"
        }
    }

    private func attachSourceNode() {
        // Standard format: deinterleaved Float32, one buffer per channel.
        guard let format = AVAudioFormat(standardFormatWithSampleRate: te_output_sample_rate(), channels: 2) else {
            lastError = "Couldn't create the engine's audio format"
            return
        }
        let node = Self.makeSourceNode(format: format, engine: engine)
        audioEngine.attach(node)
        audioEngine.connect(node, to: audioEngine.mainMixerNode, format: format)
        sourceNode = node
    }

    /// Nonisolated so the render closure doesn't inherit main-actor isolation:
    /// CoreAudio calls it on the real-time audio thread, where Swift's
    /// isolation check would trap.
    private nonisolated static func makeSourceNode(format: AVAudioFormat, engine: EngineHandle) -> AVAudioSourceNode {
        AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self)
            else { return kAudioUnitErr_InvalidParameter }
            te_engine_render(engine.pointer, left, right, frameCount)
            return noErr
        }
    }

    private func configureSession() throws {
        #if os(iOS)
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default)
        // ~5 ms buffers: low enough to feel immediate when editing, with
        // plenty of headroom for a render that takes well under 1 ms.
        try session.setPreferredIOBufferDuration(0.005)
        try session.setActive(true)
        #endif
    }

    private func updateLatency() {
        var report = LatencyReport()
        report.engineSampleRate = te_output_sample_rate()
        #if os(iOS)
        let session = AVAudioSession.sharedInstance()
        report.hardwareSampleRate = session.sampleRate
        report.ioBufferSeconds = session.ioBufferDuration
        report.outputLatencySeconds = session.outputLatency
        #else
        report.hardwareSampleRate = audioEngine.outputNode.outputFormat(forBus: 0).sampleRate
        report.outputLatencySeconds = audioEngine.outputNode.presentationLatency
        #endif
        latency = report
    }

    // MARK: - Telemetry

    private func startPolling() {
        guard pollTask == nil else { return }
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.pollStatus()
                try? await Task.sleep(for: .milliseconds(33))
            }
        }
    }

    private func pollStatus() {
        var raw = TEStatus()
        te_engine_status(engine.pointer, &raw)
        status = EngineStatus(raw)
    }

    // MARK: - Interruptions and route changes

    private func observeSystemEvents() {
        #if os(iOS)
        let center = NotificationCenter.default
        observers.append(center.addObserver(
            forName: AVAudioSession.interruptionNotification, object: nil, queue: .main
        ) { [weak self] note in
            let rawType = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            MainActor.assumeIsolated { self?.handleInterruption(rawType) }
        })
        observers.append(center.addObserver(
            forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.updateLatency() }
        })
        observers.append(center.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.restartAudio() }
        })
        #endif
        observers.append(NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: audioEngine, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.restartAudio() }
        })
    }

    #if os(iOS)
    private func handleInterruption(_ rawType: UInt?) {
        guard let rawType, let type = AVAudioSession.InterruptionType(rawValue: rawType) else { return }
        switch type {
        case .began:
            // The system has already stopped the engine.
            isAudioRunning = false
        case .ended:
            startAudioIfNeeded()
        @unknown default:
            break
        }
    }
    #endif

    private func restartAudio() {
        audioEngine.stop()
        isAudioRunning = false
        startAudioIfNeeded()
    }
}

/// The engine pointer, shared with the audio thread. The engine itself is
/// thread-safe under its documented rules (one control thread, one audio thread).
private struct EngineHandle: @unchecked Sendable {
    let pointer: OpaquePointer
}
