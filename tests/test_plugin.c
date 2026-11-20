#include "../mac/Audio/HD60SAudio.c"

#include <assert.h>
#include <stdio.h>

static UInt32 u32(AudioObjectID obj, AudioObjectPropertySelector s, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof v;
    assert(GetPropertyData(NULL, obj, 0, &a, 0, NULL, size, &size, &v) == noErr);
    return v;
}

int main(void)
{
    AudioServerPlugInDriverRef d = HD60S_Create(kCFAllocatorDefault, kAudioServerPlugInTypeUUID);
    assert(d && HD60S_Create(kCFAllocatorDefault, kAudioServerPlugInDriverInterfaceUUID) == NULL);
    AudioServerPlugInHostInterface fake = {0};
    AudioServerPlugInHostRef h = &fake;
    assert((*d)->Initialize(d, h) == noErr);

    /* The input device is found by UID and is visible; the feed is hidden. */
    AudioObjectPropertyAddress tr = {kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0};
    CFStringRef uid = CFSTR("net.mintopia.hd60s.audio");
    AudioObjectID dev = 0;
    UInt32 size = sizeof dev;
    assert((*d)->GetPropertyData(d, kPlugIn, 0, &tr, sizeof uid, &uid, size, &size, &dev) == noErr && dev == kInDev);
    assert(u32(kInDev, kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal) == 0);
    assert(u32(kFeedDev, kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal) == 1);
    assert(u32(kInStream, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1);

    /* Streams are only listed in the device's own direction. */
    AudioObjectPropertyAddress st = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0};
    assert((*d)->GetPropertyDataSize(d, kInDev, 0, &st, 0, NULL, &size) == noErr && size == 0);
    st.mScope = kAudioObjectPropertyScopeInput;
    assert((*d)->GetPropertyDataSize(d, kInDev, 0, &st, 0, NULL, &size) == noErr && size == sizeof(AudioObjectID));
    AudioObjectPropertyAddress bogus = {'nope', kAudioObjectPropertyScopeGlobal, 0};
    assert(!(*d)->HasProperty(d, kInDev, 0, &bogus));

    /* Feed written at an output sample time comes back from the input at the same sample time,
     * across the ring wrap, and reads as silence afterwards. */
    float out[2 * 8], in[2 * 8];
    for (int i = 0; i < 16; i++)
        out[i] = (float)i;
    AudioServerPlugInIOCycleInfo cyc = {0};
    cyc.mOutputTime.mSampleTime = RING - 4;
    assert((*d)->StartIO(d, kInDev, 1) == noErr);
    (*d)->DoIOOperation(d, kFeedDev, kFeedStream, 1, kAudioServerPlugInIOOperationWriteMix, 8, &cyc, out, NULL);
    cyc.mInputTime.mSampleTime = RING - 4;
    (*d)->DoIOOperation(d, kInDev, kInStream, 1, kAudioServerPlugInIOOperationReadInput, 8, &cyc, in, NULL);
    assert(memcmp(in, out, sizeof in) == 0);
    (*d)->DoIOOperation(d, kInDev, kInStream, 1, kAudioServerPlugInIOOperationReadInput, 8, &cyc, in, NULL);
    assert(in[0] == 0 && in[15] == 0);

    /* Zero time stamps land on whole periods of the ring. */
    Float64 sample;
    UInt64 hostTime, seed;
    (*d)->GetZeroTimeStamp(d, kInDev, 1, &sample, &hostTime, &seed);
    assert(fmod(sample, RING) == 0 && hostTime >= anchor);

    puts("plugin tests passed");
    return 0;
}
