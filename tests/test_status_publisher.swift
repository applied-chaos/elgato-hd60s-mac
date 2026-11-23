import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

func signal(_ json: String?) -> [String: Any] {
    let o = try! JSONSerialization.jsonObject(with: Data((json ?? "{}").utf8)) as! [String: Any]
    return o["signal"] as! [String: Any]
}

func unsupported() {
    let p = StatusPublisher()
    var t: UInt64 = 1_000_000_000
    func frames(_ w: Int, _ h: Int, _ n: Int) {
        for _ in 0..<n {
            t += 16_000_000
            p.frame(width: w, height: h, interlaced: false, topFirst: false, bt601: false, nowNs: t)
        }
    }
    frames(1920, 1080, 30)
    frames(1280, 12, 1)
    frames(1280, 703, 1)
    p.publish(frames: 32, discarded: 0, longLines: 0, nowNs: t)
    var s = signal(p.factsJSON)
    check(s["state"] as? String == "active" && s["width"] as? Int == 1920, "runts keep the active size: \(s)")
    frames(1120, 700, 30)
    p.publish(frames: 30, discarded: 0, longLines: 0, nowNs: t)
    s = signal(p.factsJSON)
    check(s["state"] as? String == "unsupported" && s["width"] as? Int == 1120 && s["height"] as? Int == 700, "unsupported: \(s)")
    frames(1280, 720, 30)
    p.publish(frames: 30, discarded: 0, longLines: 0, nowNs: t)
    s = signal(p.factsJSON)
    check(s["state"] as? String == "active" && s["width"] as? Int == 1280, "known mode returns to active: \(s)")
    t += 2_000_000_000
    p.publish(frames: 0, discarded: 0, longLines: 0, nowNs: t)
    check(signal(p.factsJSON)["state"] as? String == "none", "no frames is no signal")
}

@main enum Tests {
    static func main() {
        let p = StatusPublisher()
        var seen: [String] = []
        p.onChange = { seen.append($0) }
        check(p.factsJSON == nil, "nothing before the first publish")
        p.frame(width: 1920, height: 1080, interlaced: false, topFirst: false, bt601: false, nowNs: 1_000_000_000)
        p.publish(frames: 1, discarded: 0, longLines: 0, nowNs: 1_500_000_000)
        check(seen.count == 1 && seen[0] == p.factsJSON, "publish pushes the facts to clients")
        check(seen[0].contains("1920"), "facts carry the signal")
        p.clear()
        check(seen.count == 2 && seen[1] == "" && p.factsJSON == nil, "stop pushes empty facts")
        unsupported()
        print("status publisher tests passed")
    }
}
