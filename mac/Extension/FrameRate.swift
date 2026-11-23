import CoreMedia
import CoreMediaIO

// Advertised camera rates. Frames are always stamped at arrival, so the choice only sets the nominal
// duration and slate pacing; a source at the other rate still flows through without drops or repeats.
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
