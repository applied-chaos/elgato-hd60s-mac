# Decision: Talk to the camera extension through custom CoreMediaIO properties

Status: accepted
Date: 2026-11-23

## Context
The app and the camera extension shared `settings.json` and `status.json` in the App Group container. The extension runs as `_cmiodalassistants` (uid 262), so `containerURL(forSecurityApplicationGroupIdentifier:)` resolves to that user's container under `/var/db/cmiodalassistants`, not the logged-in user's. Neither file ever crossed: Settings changes did nothing and the Status tab never saw the extension's facts.

## Decision
Custom device properties on the extension's CMIO device, keyed `4cc_XXXX_glob_0000` (`mac/Shared/CameraProperty.swift`), values as strings (`CFString` on the app side):

- `hset`: settings JSON (`hd60s_settings_print`/`parse`). The app sets it on launch, on every change and whenever `kCMIOHardwarePropertyDevices` changes; the extension applies it live.
- `hsta`: read-only status facts JSON, empty while no stream runs.
- `hqit`: write-only; the extension exits shortly after replying. The installer uses it before replacing a running extension.

The app finds the device by the presence of `hset`. The app stays the source of truth and keeps `settings.json` in its own container. The extension saves the last settings it was sent in its own container so a restart before the app next runs keeps them. The DriverKit user-client path is unchanged.

## Consequences
- Settings reach the extension only through the app; editing a file by hand no longer does anything.
- Status needs a CMIO round trip per poll (about once a second); cheap.
- The `4cc_` key form is not formally documented by Apple; it is the form developers report working since macOS 12.3, and macOS 14+ is reported reliable for string values. Deployment target is 26.
- Anything that can open the CMIO device can set these properties, including asking the extension to quit. Same trust level as starting a stream.
- The hardened-runtime app needs `com.apple.security.device.camera` and `NSCameraUsageDescription`: without camera permission CMIO hides the device from it. Users see one camera permission prompt.

## Supersedes
None
