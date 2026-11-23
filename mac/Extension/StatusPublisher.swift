import Foundation
import os
import Darwin

final class StatusPublisher {
    private struct State {
        var width = 0, height = 0, interlaced = false, topFirst = false, bt601 = false
        var frames = 0
        var markNs: UInt64 = 0
        var lastFrameNs: UInt64 = 0
        var fps = 0.0
        var firmware: UInt32 = 0
        var client = ""
        var hdcp = false, dvi = false
    }

    private let state = OSAllocatedUnfairLock(initialState: State())
    private let latest = OSAllocatedUnfairLock(initialState: Data?.none)
    private static let activeWithinNs: UInt64 = 1_000_000_000

    func setClient(pid: pid_t, signingID: String?) {
        var path = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        let name = proc_pidpath(pid, &path, UInt32(path.count)) > 0
            ? Self.appName(URL(fileURLWithPath: String(cString: path)))
            : Self.appName(signingID: signingID)
        state.withLock { $0.client = name }
    }

    func setFirmware(_ mcu: UInt32) { state.withLock { $0.firmware = mcu } }

    func setLink(hdcp: Bool, dvi: Bool) { state.withLock { ($0.hdcp, $0.dvi) = (hdcp, dvi) } }

    func frame(width: Int, height: Int, interlaced: Bool, topFirst: Bool, bt601: Bool, nowNs: UInt64) {
        state.withLock {
            $0.width = width
            $0.height = height
            $0.interlaced = interlaced
            $0.topFirst = topFirst
            $0.bt601 = bt601
            $0.frames += 1
            $0.lastFrameNs = nowNs
        }
    }

    func publish(frames: UInt, discarded: UInt, longLines: UInt, nowNs: UInt64) {
        let s = state.withLock { s -> State in
            let span = Double(nowNs &- s.markNs) / 1e9
            if span > 0 { s.fps = Double(s.frames) / span }
            s.frames = 0
            s.markNs = nowNs
            return s
        }
        var st = hd60s_status()
        st.present = 1
        st.access = ACCESS_OK
        st.owner = OWNER_CAMERA
        withUnsafeMutableBytes(of: &st.owner_name) { buf in
            _ = s.client.utf8CString.withUnsafeBufferPointer { src in
                memcpy(buf.baseAddress!, src.baseAddress!, min(src.count, buf.count - 1))
            }
        }
        if s.firmware != 0 {
            st.fw_valid = 1
            st.mcu_version = s.firmware
        }
        st.frames = frames
        st.discarded = discarded
        st.long_lines = longLines
        if s.hdcp {
            st.signal = SIGNAL_HDCP
        } else if s.lastFrameNs != 0, nowNs &- s.lastFrameNs < Self.activeWithinNs {
            st.signal = SIGNAL_ACTIVE
            st.width = Int32(s.width)
            st.height = Int32(s.height)
            st.interlaced = s.interlaced ? 1 : 0
            st.field_order = s.topFirst ? FIELD_TOP_FIRST : FIELD_NONE
            st.fps = s.fps
            st.colour = s.bt601 ? COLOUR_BT601 : COLOUR_BT709
            st.audio_rate = 48000
            st.audio_channels = 2
        } else {
            st.signal = SIGNAL_NONE
        }
        st.source = hd60s_link_type_from_link(hd60s_link(hdcp: s.hdcp ? 1 : 0, dvi: s.dvi ? 1 : 0, hdmi: 0), st.signal)
        let data = factsData(st)
        latest.withLock { $0 = data }
    }

    var factsJSON: String? { latest.withLock { $0.map { String(decoding: $0, as: UTF8.self) } } }

    func clear() {
        latest.withLock { $0 = nil }
        state.withLock { $0 = State() }
    }

    // Outermost app bundle, so a browser's helper process reports as the browser.
    static func appName(_ executable: URL) -> String {
        if let app = executable.pathComponents.first(where: { $0.hasSuffix(".app") }) { return String(app.dropLast(4)) }
        return executable.lastPathComponent
    }

    // "com.obsproject.obs-studio" -> "obs-studio" when the process path is not visible from the sandbox.
    static func appName(signingID: String?) -> String { signingID?.split(separator: ".").last.map(String.init) ?? "" }
}
