import CoreMediaIO
import Foundation

// App end of the custom CMIO properties the camera extension publishes on its device.
enum CameraLink {
    private static let system = CMIOObjectID(kCMIOObjectSystemObject)

    private static func address(_ selector: UInt32) -> CMIOObjectPropertyAddress {
        CMIOObjectPropertyAddress(mSelector: selector, mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
                                  mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
    }

    /// The HD60 S camera device, found by the property only our extension carries.
    static func device() -> CMIOObjectID? {
        var a = address(UInt32(kCMIOHardwarePropertyDevices))
        var size: UInt32 = 0
        guard CMIOObjectGetPropertyDataSize(system, &a, 0, nil, &size) == noErr, size > 0 else { return nil }
        var ids = [CMIOObjectID](repeating: 0, count: Int(size) / MemoryLayout<CMIOObjectID>.size)
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(system, &a, 0, nil, size, &used, &ids) == noErr else { return nil }
        var probe = address(CameraProperty.settings.selector)
        return ids.first { CMIOObjectHasProperty($0, &probe) }
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
        var value = text as CFString
        return CMIOObjectSetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<CFString>.size), &value) == noErr
    }

    /// Calls `changed` on `queue` whenever cameras come or go, including the extension restarting.
    static func watchDevices(queue: DispatchQueue, _ changed: @escaping () -> Void) {
        var a = address(UInt32(kCMIOHardwarePropertyDevices))
        CMIOObjectAddPropertyListenerBlock(system, &a, queue) { _, _ in changed() }
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
