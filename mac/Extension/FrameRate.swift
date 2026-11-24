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

// With audio in the session, AVCaptureSession replaces each video pts with the previous pts plus its duration while
// they agree within about 15 ms, so durations must add up to real time or video drifts off audio.
struct FrameClock {
    private static let correctionGain = 1.0 / 4, periodGain = correctionGain * correctionGain / 4, resetNs: Int64 = 100_000_000
    private var nextNs: Int64 = 0, lastArrivalNs: Int64 = 0, periodNs = 1e9 / 60, previousWasResync = false

    mutating func stamp(arrivalNs: UInt64) -> (pts: CMTime, duration: CMTime, resync: Bool) {
        let now = Int64(arrivalNs), gap = now - lastArrivalNs
        var e = Double(now - nextNs)
        let resync = lastArrivalNs == 0 || gap > Self.resetNs || abs(e) > periodNs * 0.4
        if resync {
            let rateChanged = previousWasResync && gap < Self.resetNs
            if rateChanged { periodNs = Double(gap) }
            nextNs = now
            e = 0
        }
        previousWasResync = resync
        lastArrivalNs = now
        periodNs += Self.periodGain * e
        let correctionNs = Self.correctionGain * e
        let pts = nextNs, duration = Int64((periodNs + correctionNs).rounded())
        nextNs = pts + duration
        return (CMTime(value: pts, timescale: 1_000_000_000), CMTime(value: duration, timescale: 1_000_000_000), resync)
    }
}
