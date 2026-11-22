import Foundation

enum PreviewFacts: String {
    case live, sd, nosig, usb2, busy, none, unsupported

    var view: StatusView {
        let stereo = #"{"rate":48000,"channels":2}"#
        func facts(speed: String = "super", port: String = "2-2", present: Bool = true, owner: String = "cli",
                   signal: String, audio: String = "null", frames: Int = 0, discarded: Int = 0) -> DeviceFacts {
            let json = """
            {"device":{"present":\(present),"port":"\(port)","speed":"\(speed)","access":"ok","firmware":\(present ? "\"11.03.11\"" : "null"),
            "owner":{"kind":"\(owner)","pid":null,"name":null}},"signal":\(signal),"audio":\(audio),
            "parser":{"frames":\(frames),"discarded":\(discarded),"runts":0,"long_lines":0},"warnings":[],"notes":[]}
            """
            return try! DeviceFacts(json: Data(json.utf8))
        }
        func active(_ w: Int, _ h: Int, _ interlaced: Bool, _ fps: Double, _ colour: String) -> String {
            #"{"state":"active","width":\#(w),"height":\#(h),"interlaced":\#(interlaced),"field_order":"\#(interlaced ? "top_first" : "none")","fps":\#(fps),"colour":"\#(colour)"}"#
        }
        switch self {
        case .live: return StatusView(facts(signal: active(1920, 1080, false, 59.94, "bt709"), audio: stereo, frames: 18204), usedBy: nil)
        case .sd: return StatusView(facts(signal: active(720, 576, true, 25, "bt601"), audio: stereo, frames: 1499, discarded: 3), usedBy: nil)
        case .nosig: return StatusView(facts(signal: #"{"state":"none"}"#), usedBy: nil)
        case .usb2: return StatusView(facts(speed: "high", port: "2-2.1", signal: active(1920, 1080, false, 59.94, "bt709"), audio: stereo, frames: 2528, discarded: 412), usedBy: nil)
        case .busy: return StatusView(facts(owner: "camera_extension", signal: active(1920, 1080, false, 59.94, "bt709"), audio: stereo), usedBy: "OBS")
        case .unsupported: return StatusView(facts(signal: #"{"state":"unsupported","width":1920,"height":1200}"#), usedBy: nil)
        case .none: return StatusView(facts(port: "", present: false, signal: #"{"state":"unknown"}"#), usedBy: nil)
        }
    }
}
