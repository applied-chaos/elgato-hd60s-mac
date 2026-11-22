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
        do {
            try device.addStream(streamSource.stream)
        } catch {
            fatalError("addStream: \(error)")
        }
    }

    var availableProperties: Set<CMIOExtensionProperty> {
        [.deviceTransportType, .deviceModel, .deviceLinkedCoreAudioDeviceUID]
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
            p.linkedCoreAudioDeviceUID = "net.mintopia.hd60s.audio"
        }
        return p
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}
}

final class StreamSource: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    private let formatList: [CMIOExtensionStreamFormat]
    private let rate = OSAllocatedUnfairLock(initialState: FrameRate.ntsc)
    private var pool: CVPixelBufferPool!
    private let stop = UnsafeMutablePointer<sig_atomic_t>.allocate(capacity: 1)
    private var worker: Thread?
    private let finished = DispatchSemaphore(value: 0)
    private let parser = UnsafeMutablePointer<hd60s_parser>.allocate(capacity: 1)
    private let deinterlacer = UnsafeMutablePointer<deint>.allocate(capacity: 1)
    private let lastFrameNs = OSAllocatedUnfairLock(initialState: UInt64(0))
    private var slate: CVPixelBuffer?
    private var timer: DispatchSourceTimer?
    private var ticks = 0
    private var scaler: VTPixelTransferSession?

    override init() {
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_422YpCbCr8_yuvs,
                                       width: width, height: height, extensions: nil, formatDescriptionOut: &desc)
        formatList = FrameRate.all.map {
            CMIOExtensionStreamFormat(formatDescription: desc!, maxFrameDuration: $0.duration,
                                      minFrameDuration: $0.duration, validFrameDurations: nil)
        }
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_422YpCbCr8_yuvs,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs as CFDictionary, &pool)
        super.init()
        parser.initialize(to: hd60s_parser())
        deinterlacer.initialize(to: deint())
        slate = makeSlate()
        stream = CMIOExtensionStream(localizedName: "Elgato HD60 S", streamID: UUID(), direction: .source,
                                     clockType: .hostTime, source: self)
    }

    var formats: [CMIOExtensionStreamFormat] { formatList }

    var availableProperties: Set<CMIOExtensionProperty> { [.streamActiveFormatIndex, .streamFrameDuration] }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let p = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) {
            p.activeFormatIndex = rate.withLock { $0.index }
        }
        if properties.contains(.streamFrameDuration) {
            p.frameDuration = rate.withLock { $0.duration }
        }
        return p
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        let picked = streamProperties.activeFormatIndex.flatMap { FrameRate(index: $0) }
            ?? streamProperties.frameDuration.flatMap { FrameRate(duration: $0) }
        guard let picked, rate.withLock({ let changed = $0 != picked; $0 = picked; return changed }) else { return }
        log.info("rate set to \(picked.duration.seconds, format: .fixed(precision: 4)) s")
        if timer != nil { restartTimer() }
    }

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }

    func startStream() throws {
        guard worker == nil else { return }
        stop.pointee = 0
        audio_feed_start()
        let t = Thread { [unowned self] in run() }
        t.name = "hd60s capture"
        worker = t
        t.start()
        lastFrameNs.withLock { $0 = 0 }
        restartTimer()
    }

    private func restartTimer() {
        timer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "hd60s slate"))
        let period = rate.withLock { $0.duration.seconds }
        timer.schedule(deadline: .now(), repeating: .nanoseconds(Int(period * 1e9)), leeway: .milliseconds(1))
        timer.setEventHandler { [unowned self] in tick() }
        timer.resume()
        self.timer = timer
    }

    func stopStream() throws {
        guard worker != nil else { return }
        timer?.cancel()
        timer = nil
        stop.pointee = 1
        finished.wait()
        audio_feed_stop()
        if let scaler { VTPixelTransferSessionInvalidate(scaler) }
        scaler = nil
        worker = nil
    }

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
            var d = hd60s()
            if hd60s_open(&d) == 0 && hd60s_start(&d) == 0 {
                log.info("capture started")
                parser_init(parser, buf, cap, { ctx, frame, w, h, interlaced in
                    Unmanaged<StreamSource>.fromOpaque(ctx!).takeUnretainedValue().send(UnsafeMutablePointer(mutating: frame!), Int(w), Int(h), interlaced != 0)
                }, { _, pcm, len in audio_feed_push(pcm, len) }, ctx)
                hd60s_stream(&d, parser, stop, 0)
            } else {
                log.error("device open failed")
            }
            hd60s_stop(&d)
            hd60s_close(&d)
            for _ in 0..<10 where stop.pointee == 0 {
                usleep(100_000)
            }
        }
    }

    // Frames of any known mode are scaled into the 1080p output, aspect preserved; runts from a mode switch are dropped.
    private func send(_ frame: UnsafeMutablePointer<UInt8>, _ w: Int, _ h: Int, _ interlaced: Bool) {
        guard let r = fitRect(width: w, height: h) else { return }
        if interlaced { deint_frame(deinterlacer, frame, Int32(w), Int32(h)) }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) == kCVReturnSuccess, let pb else { return }
        CVPixelBufferLockBaseAddress(pb, [])
        let dst = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        let dstStride = CVPixelBufferGetBytesPerRow(pb)
        let rowBytes = w * 2
        if r.w == w && r.h == h {
            for y in 0..<h {
                (dst + y * dstStride).update(from: frame + y * rowBytes, count: rowBytes)
            }
        } else if !scale(frame, w, h, into: dst + r.y * dstStride + r.x * 2, stride: dstStride, r, clear: dst) {
            CVPixelBufferUnlockBaseAddress(pb, [])
            return
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        tag709(pb)
        lastFrameNs.withLock { $0 = hostNs() }
        emit(pb)
    }

    // Wraps the parser buffer and the target rectangle as pixel buffers for VideoToolbox; bars are filled black.
    // #10: tag the source buffer BT.601 for SD here and VideoToolbox converts to the 709 output.
    private func scale(_ frame: UnsafePointer<UInt8>, _ w: Int, _ h: Int, into at: UnsafeMutablePointer<UInt8>,
                       stride: Int, _ r: Rect, clear base: UnsafeMutablePointer<UInt8>) -> Bool {
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
        tag709(out)
        return VTPixelTransferSessionTransferImage(scaler, from: src, to: out) == noErr
    }

    private func tick() {
        if let slate, hostNs() &- lastFrameNs.withLock({ $0 }) > noFramesTimeoutNs {
            emit(slate)
        }
        ticks += 1
        if ticks % statsEveryTicks == 0 {
            // ponytail: unsynchronised reads of counters the capture thread writes; fine for logging.
            let p = parser.pointee
            log.info("parser: \(p.frames) frames, \(p.discarded) discarded, \(p.long_total) long lines kept")
        }
    }

    private func makeSlate() -> CVPixelBuffer? {
        guard let url = Bundle.main.url(forResource: "Slate", withExtension: "png"),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            log.error("slate image missing")
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

    private func hostNs() -> UInt64 { UInt64(CMClockGetTime(CMClockGetHostTimeClock()).seconds * 1e9) }

    private func emit(_ pb: CVPixelBuffer) {
        let now = CMClockGetTime(CMClockGetHostTimeClock())
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
