import CoreAudio
import CoreMediaIO
import CoreVideo
import Foundation
import os

private let width: Int32 = 1920
private let height: Int32 = 1080
private let frameDuration = CMTime(value: 1001, timescale: 60000)
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

    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel] }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        let p = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            p.transportType = Int(kAudioDeviceTransportTypeUSB)
        }
        if properties.contains(.deviceModel) {
            p.model = "Game Capture HD60 S"
        }
        return p
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}
}

final class StreamSource: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    private let format: CMIOExtensionStreamFormat
    private var pool: CVPixelBufferPool!
    private let stop = UnsafeMutablePointer<sig_atomic_t>.allocate(capacity: 1)
    private var worker: Thread?
    private let finished = DispatchSemaphore(value: 0)

    override init() {
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_422YpCbCr8_yuvs,
                                       width: width, height: height, extensions: nil, formatDescriptionOut: &desc)
        format = CMIOExtensionStreamFormat(formatDescription: desc!, maxFrameDuration: frameDuration,
                                           minFrameDuration: frameDuration, validFrameDurations: nil)
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_422YpCbCr8_yuvs,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs as CFDictionary, &pool)
        super.init()
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
            p.frameDuration = frameDuration
        }
        return p
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {}

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }

    func startStream() throws {
        guard worker == nil else { return }
        stop.pointee = 0
        let t = Thread { [unowned self] in run() }
        t.name = "hd60s capture"
        worker = t
        t.start()
    }

    func stopStream() throws {
        guard worker != nil else { return }
        stop.pointee = 1
        finished.wait()
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
                var parser = hd60s_parser()
                parser_init(&parser, buf, cap, { ctx, frame, w, h, _ in
                    Unmanaged<StreamSource>.fromOpaque(ctx!).takeUnretainedValue().send(frame!, Int(w), Int(h))
                }, { _, _, _ in }, ctx)
                hd60s_stream(&d, &parser, stop, 0)
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

    // ponytail: only 1920x1080 frames are forwarded; other modes need extra stream formats.
    private func send(_ frame: UnsafePointer<UInt8>, _ w: Int, _ h: Int) {
        guard w == Int(width), h == Int(height) else { return }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) == kCVReturnSuccess, let pb else { return }
        CVPixelBufferLockBaseAddress(pb, [])
        let dst = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        let dstStride = CVPixelBufferGetBytesPerRow(pb)
        let rowBytes = w * 2
        for y in 0..<h {
            (dst + y * dstStride).update(from: frame + y * rowBytes, count: rowBytes)
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)

        let now = CMClockGetTime(CMClockGetHostTimeClock())
        var timing = CMSampleTimingInfo(duration: frameDuration, presentationTimeStamp: now, decodeTimeStamp: .invalid)
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
