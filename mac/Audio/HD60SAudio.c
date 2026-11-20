// Core Audio server plug-in: a visible input device "Elgato HD60 S" and a hidden output
// device "HD60S feed". The camera extension plays HDMI audio into the feed; both devices
// share one clock and one ring indexed by sample time, so the input reads what was fed.
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <string.h>

enum { kPlugIn = kAudioObjectPlugInObject, kInDev, kInStream, kFeedDev, kFeedStream };

#define RATE 48000.0
#define CHANNELS 2
#define RING 16384 /* frames; also the zero time stamp period */
#define CLOCK_DOMAIN 0x48603053

static AudioServerPlugInHostRef host;
static uint64_t anchor;
static double ticks_per_frame;
static _Atomic int running[kFeedStream + 1];
static float ring[RING * CHANNELS];

static const AudioStreamBasicDescription format = {
    .mSampleRate = RATE,
    .mFormatID = kAudioFormatLinearPCM,
    .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
    .mBytesPerPacket = CHANNELS * sizeof(float),
    .mFramesPerPacket = 1,
    .mBytesPerFrame = CHANNELS * sizeof(float),
    .mChannelsPerFrame = CHANNELS,
    .mBitsPerChannel = 32,
};

static int is_device(AudioObjectID o) { return o == kInDev || o == kFeedDev; }
static int is_stream(AudioObjectID o) { return o == kInStream || o == kFeedStream; }

// Writes one property value; with out == NULL only reports its size. Returns an error for
// properties the object does not have, which also answers HasProperty.
static OSStatus get(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q,
                    UInt32 max, void *out, UInt32 *size)
{
#define PUT(type, value)                                          \
    do {                                                          \
        *size = sizeof(type);                                     \
        if (out) {                                                \
            if (max < sizeof(type))                               \
                return kAudioHardwareBadPropertySizeError;        \
            *(type *)out = (value);                               \
        }                                                         \
        return noErr;                                             \
    } while (0)
#define PUT_IDS(...)                                                              \
    do {                                                                          \
        AudioObjectID ids[] = {__VA_ARGS__};                                      \
        UInt32 n = sizeof ids / sizeof ids[0];                                    \
        if (out && max / sizeof(AudioObjectID) < n)                               \
            n = max / sizeof(AudioObjectID);                                      \
        *size = n * sizeof(AudioObjectID);                                        \
        if (out)                                                                  \
            memcpy(out, ids, *size);                                              \
        return noErr;                                                             \
    } while (0)
#define PUT_NONE() \
    do {           \
        *size = 0; \
        return noErr; \
    } while (0)

    AudioObjectPropertySelector s = a->mSelector;
    if (s == kAudioObjectPropertyBaseClass)
        PUT(AudioClassID, kAudioObjectClassID);
    if (s == kAudioObjectPropertyManufacturer)
        PUT(CFStringRef, CFSTR("Elgato"));

    if (obj == kPlugIn) {
        switch (s) {
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: PUT_IDS(kInDev, kFeedDev);
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID dev = kAudioObjectUnknown;
            if (qsize == sizeof(CFStringRef) && q) {
                CFStringRef uid = *(const CFStringRef *)q;
                if (CFEqual(uid, CFSTR("net.mintopia.hd60s.audio")))
                    dev = kInDev;
                else if (CFEqual(uid, CFSTR("net.mintopia.hd60s.feed")))
                    dev = kFeedDev;
            }
            PUT(AudioObjectID, dev);
        }
        }
    } else if (is_device(obj)) {
        int in = obj == kInDev;
        AudioObjectID stream = in ? kInStream : kFeedStream;
        AudioObjectPropertyScope own = in ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
        int scoped = a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == own;
        switch (s) {
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: PUT(CFStringRef, in ? CFSTR("Elgato HD60 S") : CFSTR("HD60S feed"));
        case kAudioDevicePropertyDeviceUID:
            PUT(CFStringRef, in ? CFSTR("net.mintopia.hd60s.audio") : CFSTR("net.mintopia.hd60s.feed"));
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR("net.mintopia.hd60s.model"));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: PUT_IDS(obj);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, CLOCK_DOMAIN);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, running[obj] > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, !in);
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, RING);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, RATE);
        case kAudioDevicePropertyAvailableNominalSampleRates: PUT(AudioValueRange, ((AudioValueRange){RATE, RATE}));
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *size = 2 * sizeof(UInt32);
            if (out) {
                if (max < *size)
                    return kAudioHardwareBadPropertySizeError;
                ((UInt32 *)out)[0] = 1;
                ((UInt32 *)out)[1] = 2;
            }
            return noErr;
        case kAudioObjectPropertyControlList: PUT_NONE();
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            if (scoped)
                PUT_IDS(stream);
            PUT_NONE();
        }
    } else if (is_stream(obj)) {
        int in = obj == kInStream;
        switch (s) {
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, in ? kInDev : kFeedDev);
        case kAudioObjectPropertyOwnedObjects: PUT_NONE();
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, in);
        case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, format);
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            PUT(AudioStreamRangedDescription, ((AudioStreamRangedDescription){format, {RATE, RATE}}));
        }
    } else {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
#undef PUT
#undef PUT_IDS
#undef PUT_NONE
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{
    (void)d, (void)pid;
    UInt32 size;
    CFStringRef none = CFSTR("");
    return get(obj, a, sizeof none, &none, 0, NULL, &size) == noErr;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *settable)
{
    if (!HasProperty(d, obj, pid, a))
        return kAudioHardwareUnknownPropertyError;
    *settable = false;
    return noErr;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 *size)
{
    (void)d, (void)pid;
    return get(obj, a, qsize, q, 0, NULL, size);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 max,
                                UInt32 *size, void *out)
{
    (void)d, (void)pid;
    return get(obj, a, qsize, q, max, out, size);
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 size,
                                const void *data)
{
    (void)d, (void)obj, (void)pid, (void)a, (void)qsize, (void)q, (void)size, (void)data;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d, (void)client;
    if (!is_device(dev))
        return kAudioHardwareBadObjectError;
    if (atomic_fetch_add(&running[dev], 1) == 0 && dev == kInDev)
        memset(ring, 0, sizeof ring);
    return noErr;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d, (void)client;
    if (!is_device(dev))
        return kAudioHardwareBadObjectError;
    atomic_fetch_sub(&running[dev], 1);
    return noErr;
}

// Both devices tick from one anchor on the host clock, so their sample times line up and
// match the camera's host-time video timestamps.
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *sample,
                                 UInt64 *hostTime, UInt64 *seed)
{
    (void)d, (void)dev, (void)client;
    double period = RING * ticks_per_frame;
    uint64_t n = (uint64_t)((double)(mach_absolute_time() - anchor) / period);
    *sample = (Float64)n * RING;
    *hostTime = anchor + (uint64_t)((double)n * period);
    *seed = 1;
    return noErr;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                  Boolean *will, Boolean *inPlace)
{
    (void)d, (void)client;
    *will = (dev == kInDev && op == kAudioServerPlugInIOOperationReadInput) ||
            (dev == kFeedDev && op == kAudioServerPlugInIOOperationWriteMix);
    *inPlace = true;
    return noErr;
}

static OSStatus BeginEndIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
                           const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)d, (void)dev, (void)client, (void)op, (void)frames, (void)cycle;
    return noErr;
}

// ponytail: no lock between the feed writer and input reader; they touch ring regions about two IO
// buffers apart, so a race only costs a glitch. Add a seqlock if glitches show up.
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client,
                              UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main,
                              void *secondary)
{
    (void)d, (void)stream, (void)client, (void)secondary;
    float *buf = main;
    if (dev == kInDev && op == kAudioServerPlugInIOOperationReadInput) {
        uint64_t t = (uint64_t)cycle->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < frames; i++) {
            float *slot = &ring[(t + i) % RING * CHANNELS];
            memcpy(&buf[i * CHANNELS], slot, CHANNELS * sizeof(float));
            memset(slot, 0, CHANNELS * sizeof(float)); /* silence once the feed stops */
        }
    } else if (dev == kFeedDev && op == kAudioServerPlugInIOOperationWriteMix) {
        uint64_t t = (uint64_t)cycle->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < frames; i++)
            memcpy(&ring[(t + i) % RING * CHANNELS], &buf[i * CHANNELS], CHANNELS * sizeof(float));
    }
    return noErr;
}

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef h)
{
    (void)d;
    host = h;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_per_frame = 1e9 / RATE * tb.denom / tb.numer;
    anchor = mach_absolute_time();
    return noErr;
}

static OSStatus Unsupported(void) { return kAudioHardwareUnsupportedOperationError; }

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *client, AudioObjectID *out)
{
    (void)d, (void)desc, (void)client, (void)out;
    return Unsupported();
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev)
{
    (void)d, (void)dev;
    return Unsupported();
}

static OSStatus DeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *c)
{
    (void)d, (void)dev, (void)c;
    return noErr;
}

static OSStatus ConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
    (void)d, (void)dev, (void)action, (void)info;
    return noErr;
}

static ULONG Retain(void *d) { (void)d; return 1; }

static AudioServerPlugInDriverInterface interface;
static AudioServerPlugInDriverInterface *interface_ptr = &interface;

static HRESULT QueryInterface(void *d, REFIID iid, LPVOID *out)
{
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    int ok = CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(req);
    *out = ok ? d : NULL;
    return ok ? S_OK : E_NOINTERFACE;
}

static AudioServerPlugInDriverInterface interface = {
    NULL, QueryInterface, Retain, Retain,
    Initialize, CreateDevice, DestroyDevice, DeviceClient, DeviceClient, ConfigChange, ConfigChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginEndIO, DoIOOperation, BeginEndIO,
};

void *HD60S_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? &interface_ptr : NULL;
}
