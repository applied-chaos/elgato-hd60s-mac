# Decision: The camera extension holds the device to keep HDMI passthrough running

Status: accepted
Date: 2026-11-25

## Context
HDMI passthrough is driven from the host: the IT66121 transmitter state machine, its HDCP handshake with the TV and its mode all run in `hd60s_policy_tick`. The camera extension only opened the device while a client streamed, and on stop muted the transmitter and dropped GPIO 52, so the TV went black whenever no app used the camera. With nothing driving it, the MCU firmware's own passthrough cycles black and off for an HDCP source (PS5, checked 2026-11-25), so handing back to the firmware cannot give HDCP passthrough.

## Decision
The userspace camera extension opens the device when it launches and keeps it open, reconnecting as before. Video over USB runs only while a camera client streams and the source is not HDCP. Otherwise the stream drains and the receiver leaves capture mode, so the transmitter keeps passing the source through, taking its mode from the source's AVI InfoFrame VIC. `hd60s_video_next` holds that rule; the receiver leaves capture mode only once no transfer is in flight, so decrypted video never reaches USB.

The DriverKit flavour is unchanged: its driver extension owns USB.

## Consequences
- The TV shows the source whenever the device is plugged in and the extension is loaded, HDCP sources included.
- The device is always claimed: `hd60s capture` and `hd60s status` report it busy while the extension runs. Status is available through the app (ADR 0001).
- The extension polls the device about four times a second even with no client; it also retries opening once a second while unplugged.
- Audio still flows only while a camera client streams.
- While the device is open the extension holds an XPC transaction, so launchd (`EnablePressuredExit`, set by registerassistantservice) does not reclaim it under memory pressure, and relaunches it if it crashes.
- Once launched, registerassistantservice keeps the extension running with no camera client, respawning it within milliseconds of an exit. Launch at login relies on the system's own CMIO clients (Control Center, avconferenced); not yet confirmed after a reboot. If it is not, add an SMAppService login item that enumerates CMIO devices once.

## Supersedes
None
