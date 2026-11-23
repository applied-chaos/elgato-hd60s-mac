import CoreMediaIO
import Foundation

enum CameraLink {
    private static let system = CMIOObjectID(kCMIOObjectSystemObject)

    private static func address(_ selector: UInt32) -> CMIOObjectPropertyAddress {
        CMIOObjectPropertyAddress(mSelector: selector, mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
                                  mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
    }

    static func device() -> CMIOObjectID? {
        var a = address(UInt32(kCMIOHardwarePropertyDevices))
        var size: UInt32 = 0
        guard CMIOObjectGetPropertyDataSize(system, &a, 0, nil, &size) == noErr, size > 0 else { return nil }
        var ids = [CMIOObjectID](repeating: 0, count: Int(size) / MemoryLayout<CMIOObjectID>.size)
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(system, &a, 0, nil, size, &used, &ids) == noErr else { return nil }
        var ours = address(CameraProperty.settings.selector)
        return ids.first { CMIOObjectHasProperty($0, &ours) }
    }

    static func get(_ p: CameraProperty) -> String? {
        guard let id = device() else { return nil }
        var a = address(p.selector)
        var value: Unmanaged<CFString>?
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<CFString>.size), &used, &value) == noErr else { return nil }
        return value?.takeRetainedValue() as String?
    }

    @discardableResult
    static func set(_ p: CameraProperty, _ text: String) -> Bool {
        guard let id = device() else { return false }
        var a = address(p.selector)
        let cf = text as CFString
        var value = Unmanaged.passUnretained(cf)
        return withExtendedLifetime(cf) {
            CMIOObjectSetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<CFString>.size), &value) == noErr
        }
    }

    static func watchDevices(queue: DispatchQueue, onDevicesChanged: @escaping () -> Void) {
        var a = address(UInt32(kCMIOHardwarePropertyDevices))
        CMIOObjectAddPropertyListenerBlock(system, &a, queue) { _, _ in onDevicesChanged() }
    }
}

extension CameraLink {
    static func send(_ s: hd60s_settings) {
        set(.settings, SettingsFile.json(s))
        #if DEXT
        var c = s
        _ = dext_send_settings(&c)
        #endif
    }
}
