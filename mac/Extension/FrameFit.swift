import Foundation

let outputWidth = 1920
let outputHeight = 1080

struct Rect: Equatable {
    var x, y, w, h: Int
}

// Known source sizes. HD is always 16:9; SD takes the source's AVI InfoFrame flag, 4:3 when absent.
private let hdSizes: Set<[Int]> = [[1920, 1080], [1280, 720]]
private let sdSizes: Set<[Int]> = [[720, 576], [720, 480], [1440, 480]]

// Where a w x h source lands in the 1920x1080 output, aspect preserved. Nil for sizes that are not a
// known mode (runt frames while the source switches mode).
func fitRect(width: Int, height: Int, wide: Bool = false) -> Rect? {
    let size = [width, height]
    guard hdSizes.contains(size) || sdSizes.contains(size) else { return nil }
    let (n, d) = hdSizes.contains(size) || wide ? (16, 9) : (4, 3)
    let w = min(outputWidth, outputHeight * n / d) & ~1
    let h = min(outputHeight, outputWidth * d / n) & ~1
    return Rect(x: (outputWidth - w) / 2 & ~1, y: (outputHeight - h) / 2, w: w, h: h)
}
