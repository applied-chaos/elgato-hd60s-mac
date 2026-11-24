import Foundation

let outputWidth = 1920
let outputHeight = 1080

struct Rect: Equatable {
    var x, y, w, h: Int
}

// Known source sizes (keep in step with modes[] in src/parser.c). HD is always 16:9; SD takes the source's
// AVI InfoFrame flag, 4:3 when absent; PC/DMT timings are square pixels, so they keep their own aspect.
private let hdSizes: Set<[Int]> = [[1920, 1080], [1280, 720]]
private let sdSizes: Set<[Int]> = [[720, 576], [720, 480], [1440, 480], [640, 480]]
private let pcSizes: Set<[Int]> = [
    [800, 600], [1024, 768], [1152, 864], [1280, 768], [1280, 800], [1280, 960], [1280, 1024], [1360, 768],
    [1400, 1050], [1440, 900],
]

// Where a w x h source lands in the 1920x1080 output, aspect preserved. Nil for sizes that are not a
// known mode (runt frames while the source switches mode, or a timing with no known mode).
func fitRect(width: Int, height: Int, wide: Bool = false) -> Rect? {
    let size = [width, height]
    let (n, d): (Int, Int)
    if hdSizes.contains(size) {
        (n, d) = (16, 9)
    } else if sdSizes.contains(size) {
        (n, d) = wide ? (16, 9) : (4, 3)
    } else if pcSizes.contains(size) {
        (n, d) = (width, height)
    } else {
        return nil
    }
    let w = min(outputWidth, outputHeight * n / d) & ~1
    let h = min(outputHeight, outputWidth * d / n) & ~1
    return Rect(x: (outputWidth - w) / 2 & ~1, y: (outputHeight - h) / 2, w: w, h: h)
}

// Shift added to video timestamps so apps line audio up with the picture as the HDMI stream carries it, measured
// per flavour (docs/PROTOCOL.md, "A/V alignment"). An override file in the App Group recalibrates without a rebuild.
func videoOffsetMs(override: String?, fallback: Double) -> Double {
    guard let text = override?.trimmingCharacters(in: .whitespacesAndNewlines), let v = Double(text) else { return fallback }
    return min(100, max(-100, v))
}
