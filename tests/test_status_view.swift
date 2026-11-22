import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

func json(speed: String = "super", port: String = "0-2", access: String = "ok", fw: String? = "11.03.11",
          owner: String = "cli", signal: String = "{\"state\":\"none\"}", audio: String = "null",
          frames: Int = 0, discarded: Int = 0, present: Bool = true, notes: String = "[]") -> Data {
    let fwJSON = fw.map { "\"\($0)\"" } ?? "null"
    return Data("""
    {"device":{"present":\(present),"port":"\(port)","speed":"\(speed)","access":"\(access)","firmware":\(fwJSON),
    "owner":{"kind":"\(owner)","pid":null,"name":null}},"signal":\(signal),"audio":\(audio),
    "parser":{"frames":\(frames),"discarded":\(discarded),"runts":0,"long_lines":0},"warnings":[],"notes":\(notes)}
    """.utf8)
}

func active(_ w: Int, _ h: Int, interlaced: Bool = false, order: String = "none", fps: Double, colour: String = "bt709") -> String {
    "{\"state\":\"active\",\"width\":\(w),\"height\":\(h),\"interlaced\":\(interlaced),\"field_order\":\"\(order)\",\"fps\":\(fps),\"colour\":\"\(colour)\"}"
}

let stereo = "{\"rate\":48000,\"channels\":2}"

func view(_ data: Data, usedBy: String? = nil) -> StatusView {
    guard let facts = try? DeviceFacts(json: data) else { check(false, "json parses"); exit(1) }
    return StatusView(facts, usedBy: usedBy)
}

func rows(_ r: [StatusView.Row]) -> [String] { r.map { "\($0.label)|\($0.value)|\($0.flag ? "!" : "")" } }

@main enum Tests {
    static func main() {
        let live = view(json(signal: active(1920, 1080, fps: 59.95), audio: stereo, frames: 18204))
        check(live.dot == .ok && live.title == "Live" && live.detail == "The HDMI source is coming through.", "live header")
        check(rows(live.input) == ["Resolution|1920 × 1080|", "Frame rate|59.94 fps|", "Scan|Progressive|", "Colour|BT.709|", "Audio|48 kHz stereo|"], "live input \(rows(live.input))")
        check(rows(live.connection) == ["USB|SuperSpeed, 5 Gbit/s|", "Firmware|11.03.11|", "Dropped|0 of 18,204|"], "live connection \(rows(live.connection))")

        let sd = view(json(signal: active(720, 576, interlaced: true, order: "top_first", fps: 24.55, colour: "bt601"), audio: stereo, frames: 1499, discarded: 3))
        check(rows(sd.input) == ["Resolution|720 × 576|", "Frame rate|25 fps, 50 fields|", "Scan|Interlaced, top field first|", "Colour|BT.601|", "Audio|48 kHz stereo|"], "sd input \(rows(sd.input))")
        check(rows(sd.connection).last == "Dropped|3 of 1,502|", "sd drops not flagged: \(rows(sd.connection))")

        let nosig = view(json())
        check(nosig.dot == .warn && nosig.title == "No signal", "no signal title")
        check(nosig.detail == "Check the source is on and sending HDMI. Consoles with HDCP turned on send no picture.", "no signal detail")
        check(nosig.input.isEmpty && rows(nosig.connection) == ["USB|SuperSpeed, 5 Gbit/s|", "Firmware|11.03.11|"], "no signal hides input and dropped")

        let usb2 = view(json(speed: "high", port: "0-2.1", signal: active(1920, 1080, fps: 59.94), audio: stereo, frames: 2528, discarded: 412))
        check(usb2.dot == .warn && usb2.title == "Connected over USB 2", "usb2 title")
        check(usb2.detail == "The HD60 S needs USB 3 for 1080p60. Plug it straight into a USB 3 port, not a hub.", "usb2 detail")
        check(rows(usb2.connection) == ["USB|USB 2, via hub|!", "Firmware|11.03.11|", "Dropped|412 of 2,940|!"], "usb2 connection \(rows(usb2.connection))")
        check(rows(usb2.input).count == 5, "usb2 keeps input")
        check(rows(view(json(speed: "high", port: "0-2")).connection).first == "USB|USB 2|!", "usb2 direct")

        let busy = view(json(owner: "camera_extension", signal: active(1920, 1080, fps: 59.94), audio: stereo), usedBy: "OBS")
        check(busy.dot == .ok && busy.title == "Live, in use by OBS" && busy.detail == "Only one app can use the HD60 S at a time.", "busy live")
        check(rows(busy.connection) == ["USB|SuperSpeed, 5 Gbit/s|", "Firmware|11.03.11|"], "busy without counts has no dropped")

        let blind = view(json(access: "busy", fw: nil, owner: "other", signal: "{\"state\":\"unknown\"}"), usedBy: "ffmpeg")
        check(blind.dot == .warn && blind.title == "In use by ffmpeg" && blind.input.isEmpty, "busy without facts")
        check(rows(blind.connection) == ["USB|SuperSpeed, 5 Gbit/s|"], "no firmware row when unread")
        check(view(json(access: "busy", fw: nil, owner: "other", signal: "{\"state\":\"unknown\"}")).title == "In use by another app", "anonymous owner")

        let none = view(json(port: "", fw: nil, signal: "{\"state\":\"unknown\"}", present: false))
        check(none.dot == .bad && none.title == "Not connected", "not connected")
        check(none.detail == "Plug the HD60 S into a USB 3 port on your Mac. If it is plugged in, try another cable.", "not connected detail")
        check(none.input.isEmpty && none.connection.isEmpty, "not connected hides both columns")

        let failed = view(json(access: "failed", fw: nil, signal: "{\"state\":\"unknown\"}"))
        check(failed.dot == .warn && failed.title == "Can't open the HD60 S", "open failed")

        let untested = view(json(fw: "11.03.12", signal: active(1920, 1080, fps: 60), audio: stereo, frames: 10,
                                 notes: "[{\"code\":\"untested_firmware\",\"message\":\"x\"}]"))
        check(rows(untested.connection)[1] == "Firmware|11.03.12 (untested)|", "untested firmware is a note, not amber")
        check(untested.dot == .ok, "untested firmware does not change state")

        let odd = view(json(signal: active(1280, 720, fps: 50.02), audio: "null", frames: 5))
        check(rows(odd.input) == ["Resolution|1280 × 720|", "Frame rate|50 fps|", "Scan|Progressive|", "Colour|BT.709|", "Audio|None|"], "720p50, no audio \(rows(odd.input))")
        check(view(json(signal: active(1920, 1080, interlaced: true, order: "none", fps: 29.97))).input[2].value == "Interlaced", "order unknown")
        check(view(json(signal: active(1920, 1080, fps: 37.3))).input[1].value == "37.3 fps", "unsnapped rate keeps one decimal")
        check((try? DeviceFacts(json: Data("nope".utf8))) == nil, "bad json rejected")
    }
}
