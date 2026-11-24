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
        frameClock()
        print("frame rate tests passed")
    }

    // AVCaptureSession with audio replaces each video pts with the previous pts plus its duration within avSnapMs.
    static let avSnapMs = 15.0
    static func strayMs(_ frames: [(trueNs: Double, arrivalNs: UInt64)], after skip: Int = 0) -> Double {
        var clock = FrameClock(), prev: Double?, worst = 0.0
        for (i, f) in frames.enumerated() {
            let t = clock.stamp(arrivalNs: f.arrivalNs), own = t.pts.seconds * 1e3, dur = t.duration.seconds * 1e3
            let shown = prev.map { abs(own - $0) < avSnapMs ? $0 : own } ?? own
            if i >= skip { worst = max(worst, abs(shown - f.trueNs / 1e6)) }
            prev = shown + dur
        }
        return worst
    }

    static func frameClock() {
        var rng = SystemRandomNumberGenerator()
        func frames(fps: Double, seconds: Double, from startNs: Double = 1e9, jitterMs: Double = jitterMs) -> [(trueNs: Double, arrivalNs: UInt64)] {
            (0..<Int(fps * seconds)).map {
                let t = startNs + Double($0) / fps * 1e9
                return (t, UInt64(t + Double.random(in: 0...jitterMs, using: &rng) * 1e6))
            }
        }
        let jitterMs = 2.0, loopNoiseMs = 2.0, tolerance = jitterMs / 2 + loopNoiseMs
        do { let v = strayMs(frames(fps: 60, seconds: 600), after: 60); check(v < tolerance, "60 Hz, 10 min: within jitter (\(v))") }
        do { let v = strayMs(frames(fps: 60 * 1000 / 1001, seconds: 600), after: 60); check(v < tolerance, "59.94 Hz: within jitter (\(v))") }
        do { let v = strayMs(frames(fps: 60 * (1 + 300e-6), seconds: 600), after: 60); check(v < tolerance, "60 Hz +300 ppm: tracked (\(v))") }
        do { let v = strayMs(frames(fps: 30, seconds: 60), after: 30); check(v < tolerance, "30 Hz: within jitter (\(v))") }
        do { let v = strayMs(frames(fps: 24, seconds: 60), after: 24); check(v < tolerance, "24 Hz: within jitter (\(v))") }
        let up = frames(fps: 30, seconds: 5) + frames(fps: 60, seconds: 5, from: 6e9)
        do { let v = strayMs(up, after: 150 + 60); check(v < tolerance, "30 to 60 Hz: settled within a second (\(v))") }
        let down = frames(fps: 60, seconds: 5) + frames(fps: 30, seconds: 5, from: 6e9)
        do { let v = strayMs(down, after: 300 + 30); check(v < tolerance, "60 to 30 Hz: settled within a second (\(v))") }
        let gap = frames(fps: 60, seconds: 2) + frames(fps: 60, seconds: 2, from: 4e9 + 5e6)
        do { let v = strayMs(gap, after: 120 + 60); check(v < tolerance, "resume after a gap: settled within a second (\(v))") }
        var dropped = frames(fps: 60, seconds: 20); dropped.remove(at: 600)
        do { let v = strayMs(dropped, after: 60); check(v < tolerance, "one dropped source frame: no lasting error (\(v))") }
        let steady = frames(fps: 60, seconds: 2, jitterMs: 0)
        var r = FrameClock()
        check(r.stamp(arrivalNs: 1_000_000_000).resync, "first frame starts a new line")
        check(!r.stamp(arrivalNs: 1_016_666_667).resync, "next frame continues it")
        check(r.stamp(arrivalNs: 2_000_000_000).resync, "frame after a gap starts a new line")
        var c = FrameClock(), last = CMTime.zero
        for (i, f) in steady.enumerated() {
            let t = c.stamp(arrivalNs: f.arrivalNs)
            if i > 0 { check(t.pts == last, "each pts is the previous pts plus its duration") }
            last = t.pts + t.duration
        }
    }
}
