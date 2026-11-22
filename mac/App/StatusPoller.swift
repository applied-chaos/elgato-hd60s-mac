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
    private var ringMark: (frames: UInt64, at: Date)?
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
            ringMark = nil
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
            hd60s_sample(&sampled, Self.sampleSeconds, hd60s_settings(), &stop)
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
        var info = fr_frame(), counters = fr_counters()
        guard dext_feed_peek(&info, &counters) != 0 else {
            st.access = ACCESS_BUSY
            return
        }
        if ringMark == nil {
            ringMark = (counters.frames, Date())
            Thread.sleep(forTimeInterval: 0.5)
            guard dext_feed_peek(&info, &counters) != 0 else { return st.access = ACCESS_BUSY }
        }
        let now = Date()
        let delta = counters.frames &- (ringMark?.frames ?? counters.frames)
        let span = now.timeIntervalSince(ringMark?.at ?? now)
        ringMark = (counters.frames, now)
        st.access = ACCESS_OK
        st.frames = UInt(counters.frames)
        st.discarded = UInt(counters.discarded)
        st.long_lines = UInt(counters.long_lines)
        if counters.mcu_version != 0 {
            st.fw_valid = 1
            st.mcu_version = counters.mcu_version
        }
        guard delta > 0, span > 0, info.width > 0 else {
            st.signal = SIGNAL_NONE
            return
        }
        st.signal = SIGNAL_ACTIVE
        st.width = info.width
        st.height = info.height
        st.interlaced = info.interlaced
        st.field_order = counters.top_first != 0 ? FIELD_TOP_FIRST : FIELD_NONE
        st.fps = Double(delta) / span
        st.colour = info.colour == Int32(COLOUR_BT601.rawValue) ? COLOUR_BT601 : COLOUR_BT709
        st.audio_rate = 48000
        st.audio_channels = 2
    }
    #endif

    private func decode(_ st: hd60s_status) -> DeviceFacts {
        try! DeviceFacts(json: factsData(st))
    }

    private static func readSnapshot() -> DeviceFacts? {
        guard let url = StatusSnapshot.url(),
              let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate,
              -modified.timeIntervalSinceNow < StatusSnapshot.maxAge,
              let data = try? Data(contentsOf: url) else { return nil }
        return try? DeviceFacts(json: data)
    }

    private static func otherProcess(_ st: hd60s_status) -> String? {
        guard st.owner == OWNER_OTHER else { return nil }
        let name = withUnsafeBytes(of: st.owner_name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        return name.replacingOccurrences(of: #" \(pid \d+\)$"#, with: "", options: .regularExpression)
    }
}
