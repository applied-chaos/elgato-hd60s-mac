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
        let f = FrameRate.streamFormat(desc!)
        check(f.minFrameDuration == FrameRate.ntsc.duration && f.maxFrameDuration == FrameRate.p24.duration, "range spans 59.94 to 24")
        check(f.validFrameDurations == d, "every rate is a valid duration")
        frameIntervals()
        print("frame rate tests passed")
    }

    // AVCaptureSession with audio moves each video pts to at least the previous pts plus its duration.
    static func worstPush(_ arrivalsNs: [UInt64], after skip: Int = 0) -> (pushMs: Double, durations: [Double]) {
        var clock = FrameInterval(), prev: Double?, worst = 0.0, durations = [Double]()
        for ns in arrivalsNs {
            let own = Double(ns) / 1e6, d = clock.duration(atNs: ns).seconds * 1000
            let shown = max(own, prev ?? own)
            if durations.count >= skip { worst = max(worst, shown - own) }
            prev = shown + d
            durations.append(d)
        }
        return (worst, durations)
    }

    static func frameIntervals() {
        var rng = SystemRandomNumberGenerator()
        func arrivals(fps: Double, seconds: Double, from startNs: UInt64 = 1_000_000_000, jitterMs: Double = 2) -> [UInt64] {
            (0..<Int(fps * seconds)).map { UInt64(Double(startNs) + (Double($0) / fps + Double.random(in: 0...jitterMs, using: &rng) / 1000) * 1e9) }
        }
        let p60 = worstPush(arrivals(fps: 60, seconds: 600))
        check(p60.pushMs < 2, "60 Hz with 2 ms jitter: video never pushed 2 ms ahead (\(p60.pushMs))")
        let settled = p60.durations.dropFirst(120)
        check(settled.allSatisfy { $0 > 14 && $0 < 1000 / 60.0 }, "60 Hz durations within jitter under the real interval")
        let p30 = worstPush(arrivals(fps: 30, seconds: 60))
        check(p30.pushMs < 2, "30 Hz source: no push (\(p30.pushMs))")
        check(p30.durations.dropFirst(120).allSatisfy { $0 > 30 && $0 < 1000 / 30.0 }, "30 Hz durations settle near 33 ms")
        let up = arrivals(fps: 30, seconds: 5) + arrivals(fps: 60, seconds: 5, from: 6_000_000_000)
        check(worstPush(up).pushMs < 1000 / 30.0, "30 to 60 Hz switch: under one 30 Hz frame")
        check(worstPush(up, after: 150 + 60).pushMs < 3, "30 to 60 Hz switch: back within jitter in a second")
        let gap = worstPush(arrivals(fps: 60, seconds: 2) + arrivals(fps: 60, seconds: 2, from: 4_000_000_000))
        check(gap.pushMs < 2, "resume after a gap: no push (\(gap.pushMs))")
        check(abs(worstPush([1_000_000_000]).durations[0] - 1000 / 60.0 * FrameInterval.margin) < 0.001, "first frame: shortest carried rate")
    }
}
