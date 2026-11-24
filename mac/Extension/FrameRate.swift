import CoreMedia
import CoreMediaIO

// Advertised camera rates.
enum FrameRate: CaseIterable {
    case ntsc, pal, p30, p25, p24

    var duration: CMTime {
        switch self {
        case .ntsc: CMTime(value: 1001, timescale: 60000)
        case .pal: CMTime(value: 1, timescale: 50)
        case .p30: CMTime(value: 1, timescale: 30)
        case .p25: CMTime(value: 1, timescale: 25)
        case .p24: CMTime(value: 1, timescale: 24)
        }
    }

    // One format for all rates: AVFoundation folds formats sharing a description into the first, hiding the rest.
    static func streamFormat(_ description: CMFormatDescription) -> CMIOExtensionStreamFormat {
        CMIOExtensionStreamFormat(formatDescription: description, maxFrameDuration: p24.duration,
                                  minFrameDuration: ntsc.duration, validFrameDurations: all.map(\.duration))
    }

    static var all: [FrameRate] { allCases }

    init?(duration: CMTime) {
        guard duration.isNumeric, duration.seconds > 0 else { return nil }
        let fps = 1 / duration.seconds
        self = Self.allCases.min { abs(1 / $0.duration.seconds - fps) < abs(1 / $1.duration.seconds - fps) }!
    }
}

// With audio in the session, AVCaptureSession moves each video timestamp to at least the previous one plus its
// duration, so a duration longer than the real frame interval pushes video ahead of audio.
struct FrameInterval {
    static let margin = 0.98
    private static let resetNs = 100_000_000.0
    private var lastNs: UInt64 = 0, meanNs = 0.0

    mutating func duration(atNs now: UInt64) -> CMTime {
        let gap = lastNs == 0 ? .infinity : Double(now &- lastNs)
        lastNs = now
        if gap > Self.resetNs { meanNs = 1e9 / 60 } else { meanNs += (gap - meanNs) / 32 }
        return CMTime(value: CMTimeValue(min(meanNs, gap) * Self.margin), timescale: 1_000_000_000)
    }
}
