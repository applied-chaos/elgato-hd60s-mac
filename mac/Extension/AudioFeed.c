#include "AudioFeed.h"

#include <CoreAudio/CoreAudio.h>
#include <os/log.h>

#include "ring.h"

#define TARGET 1440 /* 30 ms jitter buffer: the latency vs drift-glitch knob */

static struct ring *ring;
static AudioDeviceID dev;
static AudioDeviceIOProcID proc;

static OSStatus io(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                   AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx)
{
    (void)d, (void)now, (void)in, (void)inTime, (void)outTime, (void)ctx;
    AudioBuffer *b = &out->mBuffers[0];
    ring_pull(ring, b->mData, b->mDataByteSize / (2 * sizeof(float)), TARGET);
    return noErr;
}

void audio_feed_start(void)
{
    CFStringRef uid = CFSTR("net.mintopia.hd60s.feed");
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = sizeof dev;
    dev = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &dev) != noErr ||
        dev == kAudioObjectUnknown) {
        os_log(OS_LOG_DEFAULT, "hd60s: audio plug-in not installed");
        return;
    }
    if (!(ring = ring_create(48000)))
        return;
    OSStatus err = AudioDeviceCreateIOProcID(dev, io, NULL, &proc);
    if (!err)
        err = AudioDeviceStart(dev, proc);
    os_log(OS_LOG_DEFAULT, "hd60s: audio feed start %d", (int)err);
}

/* ponytail: drift between the HDMI and host clocks is absorbed by ring_pull dropping or padding,
 * a glitch every few minutes at worst; resample if that is audible. */
void audio_feed_push(const uint8_t *pcm, size_t len)
{
    if (ring)
        ring_push(ring, (const int16_t *)pcm, len / 4);
}

void audio_feed_stop(void)
{
    if (!ring)
        return;
    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);
    ring_free(ring);
    ring = NULL;
}
