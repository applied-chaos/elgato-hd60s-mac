import Foundation

// Mirrors the `hd60s status --json` contract (src/status.h).
struct DeviceFacts: Decodable, Equatable {
    enum Speed: String, Decodable { case unknown, low, full, high, `super`, superPlus = "super_plus" }
    enum Access: String, Decodable { case ok, busy, failed, notTried = "not_tried" }
    enum Colour: String, Decodable { case bt709, bt601, unknown }
    enum Signal: Equatable {
        case unknown, none, hdcp
        case unsupported(width: Int, height: Int)
        case active(width: Int, height: Int, interlaced: Bool, topFirst: Bool, fps: Double, colour: Colour)
    }
    enum Source: String, Decodable { case hdmi, dvi }
    struct Owner: Decodable, Equatable { var kind: String; var name: String? }
    struct Device: Decodable, Equatable {
        var present: Bool
        var port: String?
        var speed: Speed
        var access: Access
        var firmware: String?
        var owner: Owner
    }
    struct Audio: Decodable, Equatable { var rate: Int; var channels: Int }
    struct Parser: Decodable, Equatable { var frames: Int; var discarded: Int }
    struct Note: Decodable, Equatable { var code: String }

    var device: Device
    var signal: Signal
    var source: Source?
    var audio: Audio?
    var parser: Parser
    var notes: [Note]

    init(json: Data) throws { self = try JSONDecoder().decode(DeviceFacts.self, from: json) }

    private enum Keys: String, CodingKey { case device, signal, audio, parser, notes }
    private enum SignalKeys: String, CodingKey { case state, width, height, interlaced, fieldOrder = "field_order", fps, colour, source }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: Keys.self)
        device = try c.decode(Device.self, forKey: .device)
        audio = try c.decodeIfPresent(Audio.self, forKey: .audio)
        parser = try c.decode(Parser.self, forKey: .parser)
        notes = try c.decodeIfPresent([Note].self, forKey: .notes) ?? []
        let s = try c.nestedContainer(keyedBy: SignalKeys.self, forKey: .signal)
        source = try s.decodeIfPresent(Source.self, forKey: .source)
        switch try s.decode(String.self, forKey: .state) {
        case "none": signal = .none
        case "hdcp": signal = .hdcp
        case "unsupported":
            signal = .unsupported(width: try s.decode(Int.self, forKey: .width), height: try s.decode(Int.self, forKey: .height))
        case "active":
            signal = .active(width: try s.decode(Int.self, forKey: .width), height: try s.decode(Int.self, forKey: .height),
                             interlaced: try s.decode(Bool.self, forKey: .interlaced),
                             topFirst: try s.decode(String.self, forKey: .fieldOrder) == "top_first",
                             fps: try s.decode(Double.self, forKey: .fps), colour: try s.decode(Colour.self, forKey: .colour))
        default: signal = .unknown
        }
    }
}

struct StatusView: Equatable {
    enum Dot { case ok, warn, bad }
    struct Row: Equatable { var label: String, value: String, flag = false }

    var dot: Dot
    var title: String
    var detail: String
    var input: [Row] = []
    var connection: [Row] = []

    private static let droppedFramesProblemShare = 0.01
    private static let standardRates: [(Double, String)] = [(23.976, "23.976"), (24, "24"), (25, "25"), (29.97, "29.97"),
                                                           (30, "30"), (50, "50"), (59.94, "59.94"), (60, "60")]

    init(_ f: DeviceFacts, usedBy: String?) {
        guard f.device.present else {
            self = StatusView(dot: .bad, title: "Not connected",
                              detail: "Plug the HD60 S into a USB 3 port on your Mac. If it is plugged in, try another cable.")
            return
        }
        let usb2 = f.device.speed == .high || f.device.speed == .full || f.device.speed == .low
        var active = false
        if case .active(let w, let h, let interlaced, let topFirst, let fps, let colour) = f.signal {
            active = true
            input = Self.inputRows(w, h, interlaced, topFirst, fps, colour, f.audio)
            if f.source == .dvi { input.insert(Row(label: "Source", value: "DVI"), at: 0) }
        }
        connection = Self.connectionRows(f)
        if usb2 {
            dot = .warn
            title = f.device.speed == .high ? "Connected over USB 2" : "Connected over USB 1"
            detail = "The HD60 S needs USB 3 for 1080p60. Plug it straight into a USB 3 port, not a hub."
        } else if f.device.access == .failed {
            (dot, title, detail) = (.warn, "Can't open the HD60 S", "Unplug it and plug it back in.")
        } else if active {
            dot = .ok
            title = usedBy.map { "Live, in use by \($0)" } ?? "Live"
            detail = usedBy == nil ? "The HDMI source is coming through." : "Only one app can use the HD60 S at a time."
        } else if f.signal == .hdcp {
            (dot, title) = (.warn, "HDCP Protected")
            detail = "The source is encrypting its output. Turn off HDCP on the source to capture it."
        } else if case .unsupported(let w, let h) = f.signal {
            (dot, title) = (.warn, "Unsupported mode")
            detail = "The source is sending \(w) × \(h). Set it to a standard resolution such as 1080p or 720p."
        } else if f.signal == .none {
            (dot, title) = (.warn, "No signal")
            detail = "Check the source is on and sending HDMI. Consoles with HDCP turned on send no picture."
        } else {
            (dot, title) = (.warn, "In use by \(usedBy ?? "another app")")
            detail = "Only one app can use the HD60 S at a time."
        }
    }

    private init(dot: Dot, title: String, detail: String) {
        (self.dot, self.title, self.detail) = (dot, title, detail)
    }

    private static func rate(_ fps: Double) -> String {
        let nearest = standardRates.min { abs($0.0 - fps) < abs($1.0 - fps) }!
        return abs(nearest.0 - fps) / nearest.0 < 0.03 ? nearest.1 : String(format: "%.1f", fps)
    }

    private static func inputRows(_ w: Int, _ h: Int, _ interlaced: Bool, _ topFirst: Bool, _ fps: Double,
                                  _ colour: DeviceFacts.Colour, _ audio: DeviceFacts.Audio?) -> [Row] {
        let frame = rate(fps)
        let fieldRate = standardRates.first { $0.1 == frame }.map { rate($0.0 * 2) } ?? rate(fps * 2)
        return [
            Row(label: "Resolution", value: "\(w) × \(h)"),
            Row(label: "Frame rate", value: interlaced ? "\(frame) fps, \(fieldRate) fields" : "\(frame) fps"),
            Row(label: "Scan", value: interlaced ? (topFirst ? "Interlaced, top field first" : "Interlaced") : "Progressive"),
            Row(label: "Colour", value: colour == .bt709 ? "BT.709" : colour == .bt601 ? "BT.601" : "Unknown"),
            Row(label: "Audio", value: audio.map(audioText) ?? "None"),
        ]
    }

    private static func audioText(_ a: DeviceFacts.Audio) -> String {
        let khz = Double(a.rate) / 1000
        let channels = a.channels == 2 ? "stereo" : a.channels == 1 ? "mono" : "\(a.channels) channels"
        return "\(khz == khz.rounded() ? String(Int(khz)) : String(khz)) kHz \(channels)"
    }

    private static func connectionRows(_ f: DeviceFacts) -> [Row] {
        var rows = [usbRow(f.device)]
        if var fw = f.device.firmware {
            if f.notes.contains(where: { $0.code == "untested_firmware" }) { fw += " (untested)" }
            rows.append(Row(label: "Firmware", value: fw))
        }
        let total = f.parser.frames + f.parser.discarded
        if total > 0 {
            let dropped = f.parser.discarded
            rows.append(Row(label: "Dropped", value: "\(grouped(dropped)) of \(grouped(total))",
                            flag: Double(dropped) / Double(total) >= droppedFramesProblemShare))
        }
        return rows
    }

    private static func usbRow(_ d: DeviceFacts.Device) -> Row {
        switch d.speed {
        case .super: Row(label: "USB", value: "SuperSpeed, 5 Gbit/s")
        case .superPlus: Row(label: "USB", value: "SuperSpeed+, 10 Gbit/s")
        case .high: Row(label: "USB", value: "USB 2" + ((d.port ?? "").contains(".") ? ", via hub" : ""), flag: true)
        case .low, .full: Row(label: "USB", value: "USB 1", flag: true)
        case .unknown: Row(label: "USB", value: "Unknown")
        }
    }

    private static func grouped(_ n: Int) -> String {
        n.formatted(.number.grouping(.automatic).locale(Locale(identifier: "en_GB")))
    }
}
