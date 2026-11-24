import CoreAudio
import CoreMediaIO
import CoreVideo
import Foundation
import ImageIO
import os
import VideoToolbox

private let width = Int32(outputWidth)
private let height = Int32(outputHeight)
private let noFramesTimeoutNs: UInt64 = 200_000_000
private let statsEveryTicks = 600
private let publishIntervalNs: UInt64 = 1_000_000_000
private let replyBeforeExitSeconds = 0.2
private let log = Logger(subsystem: "net.mintopia.hd60s.camera", category: "capture")

final class ProviderSource: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var deviceSource: DeviceSource!

    init(clientQueue: DispatchQueue?) {
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
        deviceSource = DeviceSource()
        do {
            try provider.addDevice(deviceSource.device)
        } catch {
            fatalError("addDevice: \(error)")
        }
    }

    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) {}

    var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
        let p = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerManufacturer) {
            p.manufacturer = "Elgato"
        }
        return p
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}

final class DeviceSource: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!
    private var streamSource: StreamSource!

    override init() {
        super.init()
        device = CMIOExtensionDevice(localizedName: "Elgato HD60 S",
                                     deviceID: UUID(uuidString: "5A1F3C2E-60D5-4E8B-9C4A-0FD900740001")!,
                                     legacyDeviceID: "net.mintopia.hd60s", source: self)
        streamSource = StreamSource()
        streamSource.notify = { [unowned self] in device.notifyPropertiesChanged($0) }
        do {
            try device.addStream(streamSource.stream)
        } catch {
            fatalError("addStream: \(error)")
        }
    }

    var availableProperties: Set<CMIOExtensionProperty> {
        Set([.deviceTransportType, .deviceModel, .deviceLinkedCoreAudioDeviceUID] + CameraProperty.allCases.map(\.property))
    }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        let p = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            p.transportType = Int(kAudioDeviceTransportTypeUSB)
        }
        if properties.contains(.deviceModel) {
            p.model = "Game Capture HD60 S"
        }
        if properties.contains(.deviceLinkedCoreAudioDeviceUID) {
            #if DEXT
            p.linkedCoreAudioDeviceUID = "net.mintopia.hd60s.dext.device"
            #else
            p.linkedCoreAudioDeviceUID = "net.mintopia.hd60s.audio"
            #endif
        }
        for c in CameraProperty.allCases where properties.contains(c.property) {
            let value = switch c {
            case .settings: SettingsFile.json(streamSource.currentSettings)
            case .status: streamSource.factsJSON ?? ""
            case .quit: ""
            case .videoOffset: String(streamSource.currentVideoOffsetMs)
            }
            p.setPropertyState(c.state(value), forProperty: c.property)
        }
        return p
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {
        let set = deviceProperties.propertiesDictionary
        if let state = set[CameraProperty.settings.property] {
            guard let text = state.value as? String, let next = SettingsFile.parse(text) else {
                throw NSError(domain: NSOSStatusErrorDomain, code: Int(kCMIOHardwareIllegalOperationError))
            }
            streamSource.apply(next)
        }
        if let state = set[CameraProperty.videoOffset.property] {
            guard let text = state.value as? String else {
                throw NSError(domain: NSOSStatusErrorDomain, code: Int(kCMIOHardwareIllegalOperationError))
            }
            streamSource.overrideVideoOffset(text)
        }
        if set[CameraProperty.quit.property] != nil {
            log.info("quit requested by the installer")
            DispatchQueue.global().asyncAfter(deadline: .now() + replyBeforeExitSeconds) { exit(0) }
        }
    }
}

extension CameraProperty {
    var property: CMIOExtensionProperty { CMIOExtensionProperty(rawValue: key) }

    func state(_ value: String) -> CMIOExtensionPropertyState<AnyObject> {
        CMIOExtensionPropertyState(value: value as NSString, attributes: self == .status ? .readOnlyPropertyAttribute : nil)
    }
}

final class StreamSource: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    private let format: CMIOExtensionStreamFormat
    private let rate = OSAllocatedUnfairLock(initialState: FrameRate.ntsc)
    private var pool: CVPixelBufferPool!
    private let stop = UnsafeMutablePointer<sig_atomic_t>.allocate(capacity: 1)
    private var worker: Thread?
    private let finished = DispatchSemaphore(value: 0)
    private let parser = UnsafeMutablePointer<hd60s_parser>.allocate(capacity: 1)
    private let deinterlacer = UnsafeMutablePointer<deint>.allocate(capacity: 1)
    private let lastFrameNs = OSAllocatedUnfairLock(initialState: UInt64(0))
    private var slate: CVPixelBuffer?
    private var unsupportedSlate: CVPixelBuffer?
    private var hdcpSlate: CVPixelBuffer?
    private let hdcp = OSAllocatedUnfairLock(initialState: false)
    private var unknownRun = hd60s_unknown_run()
    private let unsupportedNs = OSAllocatedUnfairLock(initialState: UInt64(0))
    private var timer: DispatchSourceTimer?
    private let outputQueue = DispatchQueue(label: "hd60s output")
    private var ticks = 0
    #if DEXT
    private static let calibratedVideoOffsetMs = 0.0
    #else
    private static let calibratedVideoOffsetMs = 0.0
    #endif
    private let videoOffset = OSAllocatedUnfairLock(initialState: CMTime(seconds: calibratedVideoOffsetMs / 1000, preferredTimescale: 1_000_000))
    #if DEXT
    private var pushed: (settings: hd60s_settings, connections: UInt32)?
    #endif
    private var scaler: VTPixelTransferSession?
    private let statusOut = StatusPublisher()
    private var lastPublishNs: UInt64 = 0
    private let dev = UnsafeMutablePointer<hd60s>.allocate(capacity: 1)
    private let settings = OSAllocatedUnfairLock(initialState: SettingsFile.load() ?? hd60s_settings())
    var notify: (([CMIOExtensionProperty: CMIOExtensionPropertyState<AnyObject>]) -> Void)?

    override init() {
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_422YpCbCr8_yuvs,
                                       width: width, height: height, extensions: nil, formatDescriptionOut: &desc)
        format = FrameRate.streamFormat(desc!)
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_422YpCbCr8_yuvs,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs as CFDictionary, &pool)
        super.init()
        parser.initialize(to: hd60s_parser())
        dev.initialize(to: hd60s())
        deinterlacer.initialize(to: deint())
        slate = makeSlate("Slate")
        unsupportedSlate = makeSlate("Unsupported")
        hdcpSlate = makeSlate("HDCP")
        statusOut.onChange = { [unowned self] in notify?([CameraProperty.status.property: CameraProperty.status.state($0)]) }
        stream = CMIOExtensionStream(localizedName: "Elgato HD60 S", streamID: UUID(), direction: .source,
                                     clockType: .hostTime, source: self)
    }

    var formats: [CMIOExtensionStreamFormat] { [format] }

    var availableProperties: Set<CMIOExtensionProperty> { [.streamActiveFormatIndex, .streamFrameDuration] }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let p = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) {
            p.activeFormatIndex = 0
        }
        if properties.contains(.streamFrameDuration) {
            p.frameDuration = rate.withLock { $0.duration }
        }
        return p
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        guard let picked = streamProperties.frameDuration.flatMap({ FrameRate(duration: $0) }), rate.withLock({ let changed = $0 != picked; $0 = picked; return changed }) else { return }
        log.info("rate set to \(picked.duration.seconds, format: .fixed(precision: 4)) s")
        if timer != nil { restartTimer() }
    }

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        statusOut.setClient(pid: client.pid, signingID: client.signingID)
        return true
    }

    #if DEXT
    private let dextFrame = UnsafeMutablePointer<UInt8>.allocate(capacity: Int(DEXT_FRAME_BYTES))

    private var pollTimer: DispatchSourceTimer?
    // IOKit requests to a driver that is going away can block for seconds; they stay off the output queue so the
    // slate keeps flowing and clients don't stall across a replug.
    private let linkQueue = DispatchQueue(label: "hd60s driver link")
    private var linkTimer: DispatchSourceTimer?

    func startStream() throws {
        guard timer == nil else { return }
        refreshVideoOffset()
        lastFrameNs.withLock { $0 = 0 }
        // Polling only at the output rate beats against a 60 Hz source and drops about 2 frames a second.
        let poll = DispatchSource.makeTimerSource(queue: outputQueue)
        poll.schedule(deadline: .now(), repeating: .milliseconds(2), leeway: .microseconds(500))
        poll.setEventHandler { [unowned self] in pollDext() }
        poll.resume()
        pollTimer = poll
        let link = DispatchSource.makeTimerSource(queue: linkQueue)
        link.schedule(deadline: .now(), repeating: .milliseconds(100))
        link.setEventHandler { [unowned self] in
            dext_feed_maintain()
            resendSettingsOnChangeOrReconnect()
        }
        link.resume()
        linkTimer = link
        restartTimer()
    }

    func stopStream() throws {
        guard timer != nil else { return }
        timer?.cancel()
        timer = nil
        pollTimer?.cancel()
        pollTimer = nil
        linkTimer?.cancel()
        linkTimer = nil
        linkQueue.sync { dext_feed_close() }
        statusOut.clear()
        if let scaler { VTPixelTransferSessionInvalidate(scaler) }
        scaler = nil
    }
    #else
    func startStream() throws {
        guard worker == nil else { return }
        refreshVideoOffset()
        stop.pointee = 0
        audio_feed_start()
        let t = Thread { [unowned self] in run() }
        t.name = "hd60s capture"
        worker = t
        t.start()
        lastFrameNs.withLock { $0 = 0 }
        restartTimer()
    }

    func stopStream() throws {
        guard worker != nil else { return }
        timer?.cancel()
        timer = nil
        stop.pointee = 1
        finished.wait()
        audio_feed_stop()
        statusOut.clear()
        if let scaler { VTPixelTransferSessionInvalidate(scaler) }
        scaler = nil
        worker = nil
    }
    #endif

    private func restartTimer() {
        timer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: outputQueue)
        let period = rate.withLock { $0.duration.seconds }
        timer.schedule(deadline: .now(), repeating: .nanoseconds(Int(period * 1e9)), leeway: .milliseconds(1))
        timer.setEventHandler { [unowned self] in tick() }
        timer.resume()
        self.timer = timer
    }

    #if !DEXT
    // Reconnects every second so unplugging or a busy device doesn't end the stream.
    private func run() {
        let cap = Int(width) * 1440 * 2
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: cap)
        defer {
            buf.deallocate()
            finished.signal()
        }
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        while stop.pointee == 0 {
            hd60s_open_wait_ms = 4000
            let opened = hd60s_open(dev) == 0
            if opened {
                var current = settings.withLock { $0 }
                hd60s_settings_publish(dev, &current)
                var audio = audio_from_pref(current.audio)
                _ = hd60s_set_audio(dev, &audio)
            }
            if opened && hd60s_start(dev) == 0 {
                log.info("capture started")
                statusOut.setFirmware(dev.pointee.mcu_version)
                parser_init(parser, buf, cap, { ctx, frame, w, h, interlaced in
                    let me = Unmanaged<StreamSource>.fromOpaque(ctx!).takeUnretainedValue()
                    me.statusOut.frame(width: Int(w), height: Int(h), interlaced: interlaced != 0,
                                       topFirst: me.parser.pointee.top_first != 0,
                                       bt601: me.parser.pointee.colour == COLOUR_BT601, nowNs: me.hostNs())
                    me.send(UnsafeMutablePointer(mutating: frame!), Int(w), Int(h), interlaced != 0, wide: me.parser.pointee.wide != 0,
                            bt601: me.parser.pointee.colour == COLOUR_BT601)
                }, { _, pcm, len in audio_feed_push(pcm, len) }, ctx)
                hd60s_stream(dev, parser, stop, 0)
            } else {
                log.error("device open failed")
            }
            hd60s_stop(dev)
            hd60s_close(dev)
            dev.pointee.link = hd60s_link()
            for _ in 0..<10 where stop.pointee == 0 {
                usleep(100_000)
            }
        }
    }
    #endif

    // Frames of any known mode are scaled into the 1080p output, aspect preserved; runts from a mode switch are dropped.
    private func send(_ frame: UnsafeMutablePointer<UInt8>, _ w: Int, _ h: Int, _ interlaced: Bool, wide: Bool, bt601: Bool) {
        if hdcp.withLock({ $0 }) { return }
        let fit = fitRect(width: w, height: h, wide: wide)
        if hd60s_unknown_run_note(&unknownRun, hd60s_size(width: Int32(w), height: Int32(h))) >= HD60S_UNSUPPORTED_FRAMES {
            unsupportedNs.withLock { $0 = hostNs() }
        }
        guard let r = fit else { return }
        if interlaced { deint_frame(deinterlacer, frame, Int32(w), Int32(h)) }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) == kCVReturnSuccess, let pb else { return }
        CVPixelBufferLockBaseAddress(pb, [])
        let dst = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        let dstStride = CVPixelBufferGetBytesPerRow(pb)
        let rowBytes = w * 2
        if r.w == w && r.h == h && !bt601 {
            for y in 0..<h {
                (dst + y * dstStride).update(from: frame + y * rowBytes, count: rowBytes)
            }
        } else if !scale(frame, w, h, into: dst + r.y * dstStride + r.x * 2, stride: dstStride, r, clear: dst, bt601: bt601) {
            CVPixelBufferUnlockBaseAddress(pb, [])
            return
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        tag709(pb)
        lastFrameNs.withLock { $0 = hostNs() }
        emit(pb)
    }

    // Wraps the parser buffer and the target rectangle as pixel buffers for VideoToolbox; bars are filled black.
    // 601 frames (SD RGB sources, or YCbCr sources flagged 601) are tagged so VideoToolbox converts to the 709 output.
    private func scale(_ frame: UnsafePointer<UInt8>, _ w: Int, _ h: Int, into at: UnsafeMutablePointer<UInt8>,
                       stride: Int, _ r: Rect, clear base: UnsafeMutablePointer<UInt8>, bt601: Bool) -> Bool {
        if scaler == nil {
            VTPixelTransferSessionCreate(allocator: kCFAllocatorDefault, pixelTransferSessionOut: &scaler)
            if let scaler { VTSessionSetProperty(scaler, key: kVTPixelTransferPropertyKey_RealTime, value: kCFBooleanTrue) }
        }
        guard let scaler else { return false }
        var black: [UInt8] = [0x10, 0x80, 0x10, 0x80]
        memset_pattern4(base, &black, stride * outputHeight)
        var src: CVPixelBuffer?
        var out: CVPixelBuffer?
        guard CVPixelBufferCreateWithBytes(kCFAllocatorDefault, w, h, kCVPixelFormatType_422YpCbCr8_yuvs,
                                           UnsafeMutableRawPointer(mutating: frame), w * 2, nil, nil, nil, &src) == kCVReturnSuccess,
              let src,
              CVPixelBufferCreateWithBytes(kCFAllocatorDefault, r.w, r.h, kCVPixelFormatType_422YpCbCr8_yuvs,
                                           at, stride, nil, nil, nil, &out) == kCVReturnSuccess,
              let out else { return false }
        tag709(src)
        if bt601 {
            // Matrix only: primaries and transfer stay 709 so SD and HD bars keep the same RGB.
            CVBufferSetAttachment(src, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, .shouldPropagate)
        }
        tag709(out)
        return VTPixelTransferSessionTransferImage(scaler, from: src, to: out) == noErr
    }

    private func refreshLink() {
        #if DEXT
        let c = dext_feed_counters()
        let (protected, dvi) = (c.hdcp != 0, c.dvi != 0)
        #else
        // ponytail: unsynchronised read of flags the capture thread writes; a stale tick is harmless.
        let l = dev.pointee.link
        let (protected, dvi) = (l.hdcp != 0, l.dvi != 0)
        #endif
        hdcp.withLock { $0 = protected }
        statusOut.setLink(hdcp: protected, dvi: dvi)
    }

    #if DEXT
    private func pollDext() {
        var info = fr_frame()
        guard dext_feed_poll(dextFrame, Int(DEXT_FRAME_BYTES), &info) != 0 else { return }
        let c = dext_feed_counters()
        statusOut.frame(width: Int(info.width), height: Int(info.height), interlaced: info.interlaced != 0,
                        topFirst: c.top_first != 0, bt601: info.colour == Int32(COLOUR_BT601.rawValue), nowNs: hostNs())
        statusOut.setFirmware(c.mcu_version)
        send(dextFrame, Int(info.width), Int(info.height), info.interlaced != 0, wide: info.wide != 0,
             bt601: info.colour == Int32(COLOUR_BT601.rawValue))
    }
    #endif

    private func tick() {
        refreshLink()
        let protected = hdcp.withLock { $0 }
        if protected || hostNs() &- lastFrameNs.withLock({ $0 }) > noFramesTimeoutNs {
            let odd = hostNs() &- unsupportedNs.withLock({ $0 }) < 1_000_000_000
            if let s = protected ? hdcpSlate : odd ? unsupportedSlate : slate { emit(s) }
        }
        ticks += 1
        publishStatus()
        if ticks % statsEveryTicks == 0 {
            // ponytail: unsynchronised reads of counters the capture thread writes; fine for logging.
            #if DEXT
            let p = dext_feed_counters()
            let long_total = p.long_lines
            log.info("parser: \(p.frames) frames, \(p.discarded) discarded, \(long_total) long lines kept, link \(p.link.rawValue), \(p.errors) errors")
            #else
            let p = parser.pointee
            log.info("parser: \(p.frames) frames, \(p.discarded) discarded, \(p.long_total) long lines kept")
            #endif
        }
    }

    var currentSettings: hd60s_settings { settings.withLock { $0 } }

    var factsJSON: String? { statusOut.factsJSON }

    func apply(_ next: hd60s_settings) {
        var previous = settings.withLock { current in
            let p = current
            current = next
            return p
        }
        var latest = next
        guard hd60s_settings_equal(&previous, &latest) == 0 else { return }
        persistForRestart(next)
        notify?([CameraProperty.settings.property: CameraProperty.settings.state(SettingsFile.json(next))])
        #if !DEXT
        hd60s_settings_publish(dev, &latest)
        #endif
    }

    private func persistForRestart(_ s: hd60s_settings) {
        if let url = SettingsFile.url() { try? SettingsFile.write(s, to: url) }
    }

    #if DEXT
    private func resendSettingsOnChangeOrReconnect() {
        var current = currentSettings
        let connections = dext_feed_connections()
        if var last = pushed?.settings, hd60s_settings_equal(&last, &current) != 0, pushed?.connections == connections { return }
        pushed = dext_send_settings(&current) == 0 ? (current, connections) : nil
    }
    #endif

    private func publishStatus() {
        let now = hostNs()
        guard now &- lastPublishNs >= publishIntervalNs else { return }
        lastPublishNs = now
        #if DEXT
        let p = dext_feed_counters()
        statusOut.publish(frames: UInt(p.frames), discarded: UInt(p.discarded), longLines: UInt(p.long_lines), nowNs: now)
        #else
        let p = parser.pointee
        statusOut.publish(frames: p.frames, discarded: p.discarded, longLines: UInt(p.long_total), nowNs: now)
        #endif
    }

    private func makeSlate(_ name: String) -> CVPixelBuffer? {
        guard let url = Bundle.main.url(forResource: name, withExtension: "png"),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            log.error("slate image missing: \(name)")
            return nil
        }
        var rgb: CVPixelBuffer?
        var yuv: CVPixelBuffer?
        var session: VTPixelTransferSession?
        guard CVPixelBufferCreate(kCFAllocatorDefault, Int(width), Int(height), kCVPixelFormatType_32BGRA,
                                  nil, &rgb) == kCVReturnSuccess, let rgb,
              CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &yuv) == kCVReturnSuccess, let yuv,
              VTPixelTransferSessionCreate(allocator: kCFAllocatorDefault, pixelTransferSessionOut: &session) == noErr,
              let session else { return nil }
        CVPixelBufferLockBaseAddress(rgb, [])
        let ctx = CGContext(data: CVPixelBufferGetBaseAddress(rgb), width: Int(width), height: Int(height),
                            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(rgb),
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        ctx?.draw(image, in: CGRect(x: 0, y: 0, width: Int(width), height: Int(height)))
        CVPixelBufferUnlockBaseAddress(rgb, [])
        VTSessionSetProperty(session, key: kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                             value: kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTSessionSetProperty(session, key: kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                             value: kCVImageBufferColorPrimaries_ITU_R_709_2)
        VTSessionSetProperty(session, key: kVTPixelTransferPropertyKey_DestinationTransferFunction,
                             value: kCVImageBufferTransferFunction_ITU_R_709_2)
        let rc = VTPixelTransferSessionTransferImage(session, from: rgb, to: yuv)
        VTPixelTransferSessionInvalidate(session)
        guard rc == noErr else { return nil }
        tag709(yuv)
        return yuv
    }

    private func tag709(_ pb: CVPixelBuffer) {
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)
    }

    var currentVideoOffsetMs: Double { videoOffset.withLock { $0.seconds * 1000 } }

    private static var videoOffsetURL: URL? { AppGroup.container?.appendingPathComponent("video-offset-ms") }

    // Empty text clears the override. Applies from the next stream start, since a shift mid-stream would send
    // timestamps backwards.
    func overrideVideoOffset(_ text: String) {
        if let url = Self.videoOffsetURL {
            if text.isEmpty { try? FileManager.default.removeItem(at: url) } else { try? text.write(to: url, atomically: true, encoding: .utf8) }
        }
    }

    private func refreshVideoOffset() {
        let text = Self.videoOffsetURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) }
        let next = CMTime(seconds: videoOffsetMs(override: text, fallback: Self.calibratedVideoOffsetMs) / 1000, preferredTimescale: 1_000_000)
        let previous = videoOffset.withLock { old in defer { old = next }; return old }
        if previous != next { log.info("video timestamp offset \(next.seconds * 1000) ms") }
    }

    private func hostNs() -> UInt64 { UInt64(CMClockGetTime(CMClockGetHostTimeClock()).seconds * 1e9) }

    private func emit(_ pb: CVPixelBuffer) {
        let now = CMClockGetTime(CMClockGetHostTimeClock()) + videoOffset.withLock { $0 }
        var timing = CMSampleTimingInfo(duration: rate.withLock { $0.duration }, presentationTimeStamp: now, decodeTimeStamp: .invalid)
        var desc: CMFormatDescription?
        var sample: CMSampleBuffer?
        guard CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pb,
                                                           formatDescriptionOut: &desc) == noErr, let desc,
              CMSampleBufferCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pb, dataReady: true,
                                                 makeDataReadyCallback: nil, refcon: nil, formatDescription: desc,
                                                 sampleTiming: &timing, sampleBufferOut: &sample) == noErr,
              let sample else { return }
        stream.send(sample, discontinuity: [], hostTimeInNanoseconds: UInt64(now.seconds * Double(NSEC_PER_SEC)))
    }
}
