import Foundation

final class StatusPoller: ObservableObject {
    @Published private(set) var view: StatusView?
    private let pinnedPreview: PreviewFacts?

    init(preview: PreviewFacts? = nil) {
        pinnedPreview = preview
        view = preview?.view
    }

    private let queue = DispatchQueue(label: "net.mintopia.hd60s.status")
    private var timer: DispatchSourceTimer?
    private var sampledFrames: UInt = 0, sampledDiscarded: UInt = 0
    private typealias RingPoll = (number: UInt32, at: Date, size: hd60s_size, active: Bool)
    // ponytail: frames counted over a window only just tell 60 from 59.94; pass the MST3367 vsync rate
    // through the frame ring if it must be exact.
    private var ringWindow: [RingPoll] = []
    private static let fpsWindowSeconds = 30.0
    private var ringUnknown = hd60s_unknown_run()
    private static let sampleSeconds = 0.8
    private static let restSeconds = 1.0

    func start() {
        guard timer == nil, pinnedPreview == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: 1)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
        queue.async { [self] in
            sampledFrames = 0
            sampledDiscarded = 0
            ringWindow = []
            ringUnknown = hd60s_unknown_run()
            #if DEXT
            dext_feed_close()
            #endif
        }
    }

    private func tick() {
        let (facts, usedBy) = gather()
        let next = StatusView(facts, usedBy: usedBy)
        DispatchQueue.main.async { if self.view != next { self.view = next } }
    }

    private func gather() -> (DeviceFacts, String?) {
        var st = hd60s_status()
        hd60s_probe_usb(&st)
        let snapshot = Self.readSnapshot()
        let usedBy = snapshot?.device.owner.name ?? Self.otherProcess(st)
        guard st.present != 0 else { return (decode(st), nil) }

        #if DEXT
        if st.owner == OWNER_DRIVER {
            fillFromRing(&st)
            return (decode(st), snapshot?.device.owner.name)
        }
        // Opening the device here could beat the driver extension to it after a replug.
        if st.owner == OWNER_NONE { return (decode(st), nil) }
        #endif
        if let snap = snapshot, st.owner != OWNER_NONE {
            var facts = decode(st)
            facts.device.access = .ok
            facts.device.firmware = snap.device.firmware
            facts.signal = snap.signal
            facts.audio = snap.audio
            facts.parser = snap.parser
            facts.notes = snap.notes
            return (facts, usedBy)
        }
        if st.owner == OWNER_NONE {
            var sampled = hd60s_status()
            var stop: sig_atomic_t = 0
            hd60s_sample(&sampled, Self.sampleSeconds, SettingsFile.load() ?? hd60s_settings(), &stop)
            // Leaves the device free between samples so a camera client's open gets in.
            Thread.sleep(forTimeInterval: Self.restSeconds)
            sampledFrames += sampled.frames
            sampledDiscarded += sampled.discarded
            sampled.frames = sampledFrames
            sampled.discarded = sampledDiscarded
            return (decode(sampled), nil)
        }
        var facts = decode(st)
        facts.device.access = .busy
        return (facts, usedBy)
    }

    #if DEXT
    private func fillFromRing(_ st: inout hd60s_status) {
        var info = fr_frame(), counters = fr_counters(), number: UInt32 = 0
        var signal = SIGNAL_NONE
        var rate: (from: RingPoll, to: RingPoll)?
        // A fresh start polls until a stable unsupported mode can be told from runts, instead of first reading no signal.
        for poll in 0..<(ringWindow.isEmpty ? Int(HD60S_UNSUPPORTED_POLLS) + 1 : 1) {
            if poll > 0 { Thread.sleep(forTimeInterval: 0.2) }
            guard dext_feed_peek(&info, &counters, &number) != 0 else { return st.access = ACCESS_BUSY }
            let size = hd60s_size(width: info.width, height: info.height)
            let previous = ringWindow.last
            signal = hd60s_ring_signal(previous.map { $0.number != number } ?? false, counters.hdcp != 0, size, &ringUnknown)
            let now: RingPoll = (number, Date(), size, signal == SIGNAL_ACTIVE)
            if now.active, let previous, previous.active, previous.size.width == size.width, previous.size.height == size.height {
                ringWindow.append(now)
                ringWindow.removeAll { now.at.timeIntervalSince($0.at) > Self.fpsWindowSeconds }
                rate = ringWindow.count > 1 ? (ringWindow[0], now) : (previous, now)
            } else {
                ringWindow = [now]
                rate = previous.map { ($0, now) }
            }
        }
        st.access = ACCESS_OK
        st.frames = UInt(counters.frames)
        st.discarded = UInt(counters.discarded)
        st.long_lines = UInt(counters.long_lines)
        if counters.mcu_version != 0 {
            st.fw_valid = 1
            st.mcu_version = counters.mcu_version
        }
        st.signal = signal
        switch signal {
        case SIGNAL_ACTIVE:
            st.width = info.width
            st.height = info.height
            st.interlaced = info.interlaced
            st.field_order = counters.top_first != 0 ? FIELD_TOP_FIRST : FIELD_NONE
            if let rate, rate.to.at > rate.from.at {
                st.fps = Double(rate.to.number &- rate.from.number) / rate.to.at.timeIntervalSince(rate.from.at)
            }
            st.colour = info.colour == Int32(COLOUR_BT601.rawValue) ? COLOUR_BT601 : COLOUR_BT709
            st.audio_rate = 48000
            st.audio_channels = 2
        case SIGNAL_UNSUPPORTED:
            st.width = info.width
            st.height = info.height
        case SIGNAL_NONE, SIGNAL_HDCP, SIGNAL_UNKNOWN:
            break
        default:
            preconditionFailure("unknown signal \(signal.rawValue)")
        }
    }
    #endif

    private func decode(_ st: hd60s_status) -> DeviceFacts {
        try! DeviceFacts(json: factsData(st))
    }

    private static func readSnapshot() -> DeviceFacts? {
        guard let text = CameraLink.get(.status), !text.isEmpty else { return nil }
        return try? DeviceFacts(json: Data(text.utf8))
    }

    private static func otherProcess(_ st: hd60s_status) -> String? {
        guard st.owner == OWNER_OTHER else { return nil }
        let name = withUnsafeBytes(of: st.owner_name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        return name.replacingOccurrences(of: #" \(pid \d+\)$"#, with: "", options: .regularExpression)
    }
}
