import CoreMedia
import CoreMediaIO
import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        let d = FrameRate.all.map(\.duration)
        check(d == [CMTime(value: 1001, timescale: 60000), CMTime(value: 1, timescale: 50), CMTime(value: 1, timescale: 30),
                    CMTime(value: 1, timescale: 25), CMTime(value: 1, timescale: 24)], "59.94, 50 first, then 30, 25, 24")
        check(FrameRate.all.first == .ntsc, "59.94 is the default")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 60000)) == .ntsc, "exact 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 50)) == .pal, "exact 50")
        check(FrameRate(duration: CMTime(value: 1, timescale: 60)) == .ntsc, "60 snaps to 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 25)) == .p25, "exact 25")
        check(FrameRate(duration: CMTime(value: 1, timescale: 24)) == .p24, "exact 24")
        check(FrameRate(duration: CMTime(value: 1, timescale: 30)) == .p30, "exact 30")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 30000)) == .p30, "29.97 snaps to 30")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 24000)) == .p24, "23.976 snaps to 24")
        check(FrameRate(duration: CMTime(value: 1, timescale: 54)) == .pal, "nearest by fps")
        check(FrameRate(duration: CMTime(value: 1, timescale: 100)) == .ntsc, "far above snaps to 59.94")
        check(FrameRate(duration: .invalid) == nil && FrameRate(duration: .zero) == nil, "bad duration rejected")
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_422YpCbCr8_yuvs,
                                       width: 1920, height: 1080, extensions: nil, formatDescriptionOut: &desc)
        // One format: AVFoundation folds formats sharing a description into the first, hiding the other rates.
        let f = FrameRate.streamFormat(desc!)
        check(f.minFrameDuration == FrameRate.ntsc.duration && f.maxFrameDuration == FrameRate.p24.duration, "range spans 59.94 to 24")
        check(f.validFrameDurations == d, "every rate is a valid duration")
        print("frame rate tests passed")
    }
}
