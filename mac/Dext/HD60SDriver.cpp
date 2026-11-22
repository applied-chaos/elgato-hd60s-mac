#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOUserClient.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "HD60SDriver.h"

extern "C" {
#include "device.h"
#include "framering.h"
#include "link.h"
#include "parser.h"
#include "ring.h"
}

static constexpr double kSampleRate = 48000.0;
static constexpr uint32_t kChannels = 2;
static constexpr uint32_t kIOFrames = 16384;
static constexpr uint32_t kBytesPerFrame = kChannels * sizeof(float);
static constexpr size_t kAudioRingFrames = 48000;
static constexpr size_t kAudioTarget = 1440;
static constexpr uint32_t kFrameClientType = 'hd6f';
static constexpr int kXfers = 16;
static constexpr uint32_t kXferSize = 512 * 1024;
static constexpr uint64_t kPolicyMs = 100;
static constexpr uint32_t kCtrlBytes = 256;
static constexpr const char *kDeviceName = "Elgato HD60 S";

struct Xfer {
    OSSharedPtr<IOBufferMemoryDescriptor> buf;
    OSSharedPtr<OSAction> action;
    const uint8_t *data;
};

struct XferRef {
    uint32_t index, generation;
};

struct HD60SDriver_IVars {
    OSSharedPtr<IODispatchQueue> queue, usbQueue;
    OSSharedPtr<IOUserAudioDevice> device;
    OSSharedPtr<IOUserAudioStream> stream;
    OSSharedPtr<IOBufferMemoryDescriptor> ioRing;
    IOMemoryMap *ioMap = nullptr;
    OSSharedPtr<IOTimerDispatchSource> zeroTimer, policyTimer;
    OSSharedPtr<OSAction> zeroAction, policyAction;
    uint64_t periodTicks = 0, msTicks = 0, zeroHostTime = 0, zeroSample = 0;

    OSSharedPtr<IOUSBHostDevice> usb;
    OSSharedPtr<IOUSBHostInterface> intf;
    OSSharedPtr<IOUSBHostPipe> pipe;
    OSSharedPtr<IOBufferMemoryDescriptor> ctrlBuf;
    uint8_t *ctrlData = nullptr;
    Xfer xfers[kXfers];
    uint32_t generation = 0;
    bool streamFailed = false, stopping = false;

    struct hd60s dev;
    struct hd60s_parser parser;
    struct hd60s_policy policy;
    struct link link;
    uint8_t *frameBuf = nullptr;
    struct ring *audio = nullptr;

    OSSharedPtr<IOBufferMemoryDescriptor> frameMem;
    IOMemoryMap *frameMap = nullptr;
    struct fr_header *frames = nullptr;
};

static uint64_t nowMs(const HD60SDriver_IVars *iv) { return mach_absolute_time() / iv->msTicks; }

static int ctrlThunk(void *ctx, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
    return static_cast<HD60SDriver *>(ctx)->Control(in, req, val, idx, buf, len);
}

static void sleepThunk(void *, int ms) { IOSleep(static_cast<uint64_t>(ms)); }

static void frameThunk(void *ctx, const uint8_t *frame, int w, int h, int interlaced)
{
    static_cast<HD60SDriver *>(ctx)->Frame(frame, w, h, interlaced);
}

static void audioThunk(void *ctx, const uint8_t *pcm, size_t len) { static_cast<HD60SDriver *>(ctx)->Audio(pcm, len); }

static uint8_t *addressOf(IOBufferMemoryDescriptor *md)
{
    IOAddressSegment range = {};
    md->GetAddressRange(&range);
    return reinterpret_cast<uint8_t *>(range.address);
}

bool HD60SDriver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(HD60SDriver_IVars, 1);
    return ivars != nullptr;
}

void HD60SDriver::free()
{
    if (ivars) {
        if (ivars->frameBuf) IOFree(ivars->frameBuf, FR_SLOT_BYTES);
        if (ivars->audio) IOFree(ivars->audio, ring_bytes(kAudioRingFrames));
    }
    IOSafeDeleteNULL(ivars, HD60SDriver_IVars, 1);
    super::free();
}

static kern_return_t addAudioDevice(HD60SDriver *self, HD60SDriver_IVars *iv)
{
    auto name = OSSharedPtr(OSString::withCString(kDeviceName), OSNoRetain);
    auto uid = OSSharedPtr(OSString::withCString("net.mintopia.hd60s.dext.device"), OSNoRetain);
    auto model = OSSharedPtr(OSString::withCString("HD60 S"), OSNoRetain);
    auto maker = OSSharedPtr(OSString::withCString("Elgato"), OSNoRetain);
    iv->device = IOUserAudioDevice::Create(self, false, uid.get(), model.get(), maker.get(), kIOFrames);
    if (!iv->device) return kIOReturnNoMemory;
    iv->device->SetName(name.get());
    iv->device->SetSampleRate(kSampleRate);
    iv->device->SetTransportType(IOUserAudioTransportType::USB);
    iv->device->SetCanBeDefaultInputDevice(false);

    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kIOFrames * kBytesPerFrame, 0, iv->ioRing.attach());
    if (ret != kIOReturnSuccess) return ret;
    iv->ioRing->SetLength(kIOFrames * kBytesPerFrame);
    ret = iv->ioRing->CreateMapping(0, 0, 0, 0, 0, &iv->ioMap);
    if (ret != kIOReturnSuccess) return ret;

    iv->stream = IOUserAudioStream::Create(self, IOUserAudioStreamDirection::Input, iv->ioRing.get());
    if (!iv->stream) return kIOReturnNoMemory;
    IOUserAudioStreamBasicDescription format = {};
    format.mSampleRate = kSampleRate;
    format.mFormatID = IOUserAudioFormatID::LinearPCM;
    format.mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | IOUserAudioFormatFlags::FormatFlagIsPacked);
    format.mBytesPerPacket = kBytesPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    iv->stream->SetAvailableStreamFormats(&format, 1);
    iv->stream->SetCurrentStreamFormat(&format);
    ret = iv->device->AddStream(iv->stream.get());
    if (ret != kIOReturnSuccess) return ret;

    iv->device->SetIOOperationHandler(^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation op,
                                                     uint32_t frames, uint64_t sampleTime, uint64_t) {
        if (op != IOUserAudioIOOperationBeginRead || !iv->ioMap) return kIOReturnSuccess;
        auto *base = reinterpret_cast<float *>(iv->ioMap->GetAddress());
        uint32_t at = sampleTime % kIOFrames;
        uint32_t first = frames < kIOFrames - at ? frames : kIOFrames - at;
        ring_pull(iv->audio, base + at * kChannels, first, kAudioTarget);
        if (first < frames) ring_pull(iv->audio, base, frames - first, kAudioTarget);
        return kIOReturnSuccess;
    });
    return self->AddObject(iv->device.get());
}

static kern_return_t openUsb(HD60SDriver *self, HD60SDriver_IVars *iv, IOService *provider)
{
    iv->usb.reset(OSDynamicCast(IOUSBHostDevice, provider), OSRetain);
    if (!iv->usb) return kIOReturnNoDevice;
    kern_return_t ret = iv->usb->Open(self, 0, 0);
    if (ret != kIOReturnSuccess) return ret;
    ret = iv->usb->SetConfiguration(1, false);
    if (ret != kIOReturnSuccess) return ret;
    uintptr_t it = 0;
    ret = iv->usb->CreateInterfaceIterator(&it);
    if (ret != kIOReturnSuccess) return ret;
    IOUSBHostInterface *intf = nullptr;
    ret = iv->usb->CopyInterface(it, &intf);
    iv->usb->DestroyInterfaceIterator(it);
    if (ret != kIOReturnSuccess || !intf) return ret ? ret : kIOReturnNotFound;
    iv->intf.reset(intf, OSNoRetain);
    ret = iv->intf->Open(self, 0, nullptr);
    if (ret != kIOReturnSuccess) return ret;
    ret = iv->usb->CreateIOBuffer(kIOMemoryDirectionInOut, kCtrlBytes, iv->ctrlBuf.attach());
    if (ret != kIOReturnSuccess) return ret;
    iv->ctrlData = addressOf(iv->ctrlBuf.get());
    for (uint32_t i = 0; i < kXfers; i++) {
        Xfer &x = iv->xfers[i];
        ret = iv->intf->CreateIOBuffer(kIOMemoryDirectionIn, kXferSize, x.buf.attach());
        if (ret != kIOReturnSuccess) return ret;
        x.data = addressOf(x.buf.get());
        ret = self->CreateActionBulkDone(sizeof(XferRef), x.action.attach());
        if (ret != kIOReturnSuccess) return ret;
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(HD60SDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    auto *iv = ivars;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    iv->msTicks = 1000000ull * tb.denom / tb.numer;
    iv->periodTicks = static_cast<uint64_t>(1e9 * kIOFrames / kSampleRate * tb.denom / tb.numer);

    iv->queue = GetWorkQueue();
    ret = IODispatchQueue::Create("USB", 0, 0, iv->usbQueue.attach());
    if (ret != kIOReturnSuccess) return ret;
    ret = SetDispatchQueue("USB", iv->usbQueue.get());
    if (ret != kIOReturnSuccess) return ret;

    iv->frameBuf = static_cast<uint8_t *>(IOMallocZero(FR_SLOT_BYTES));
    void *audioMem = IOMallocZero(ring_bytes(kAudioRingFrames));
    if (!iv->frameBuf || !audioMem) return kIOReturnNoMemory;
    iv->audio = ring_init(audioMem, kAudioRingFrames);
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, fr_bytes(), 4096, iv->frameMem.attach());
    if (ret != kIOReturnSuccess) return ret;
    iv->frameMem->SetLength(fr_bytes());
    ret = iv->frameMem->CreateMapping(0, 0, 0, 0, 0, &iv->frameMap);
    if (ret != kIOReturnSuccess) return ret;
    iv->frames = fr_init(reinterpret_cast<void *>(iv->frameMap->GetAddress()));

    auto name = OSSharedPtr(OSString::withCString(kDeviceName), OSNoRetain);
    SetName(name.get());
    ret = addAudioDevice(this, iv);
    if (ret != kIOReturnSuccess) return ret;
    ret = openUsb(this, iv, provider);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "HD60S dext: USB open failed 0x%x", ret);
        return ret;
    }

    ret = IOTimerDispatchSource::Create(iv->queue.get(), iv->zeroTimer.attach());
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionZeroTimestampOccurred(0, iv->zeroAction.attach());
    if (ret != kIOReturnSuccess) return ret;
    iv->zeroTimer->SetHandler(iv->zeroAction.get());

    ret = IOTimerDispatchSource::Create(iv->usbQueue.get(), iv->policyTimer.attach());
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionPolicyOccurred(0, iv->policyAction.attach());
    if (ret != kIOReturnSuccess) return ret;
    iv->policyTimer->SetHandler(iv->policyAction.get());
    iv->policyTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time(), 0);
    iv->policyTimer->SetEnable(true);

    ret = RegisterService();
    os_log(OS_LOG_DEFAULT, "HD60S dext started: 0x%x", ret);
    return ret;
}

kern_return_t IMPL(HD60SDriver, Stop)
{
    auto *iv = ivars;
    if (iv->zeroTimer) iv->zeroTimer->Cancel(^{});
    if (iv->usbQueue) iv->usbQueue->DispatchSync(^{
        iv->stopping = true;
        if (iv->policyTimer) iv->policyTimer->Cancel(^{});
        if (iv->link.state == LINK_UP) TearDown(true);
    });
    if (iv->intf) iv->intf->Close(this, 0);
    if (iv->usb) iv->usb->Close(this, 0);
    if (iv->device) RemoveObject(iv->device.get());
    if (iv->ioMap) { iv->ioMap->release(); iv->ioMap = nullptr; }
    if (iv->frameMap) { iv->frameMap->release(); iv->frameMap = nullptr; iv->frames = nullptr; }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(HD60SDriver, NewUserClient)
{
    if (type != kFrameClientType) return NewUserClient(type, userClient, SUPERDISPATCH);
    IOService *client = nullptr;
    kern_return_t ret = Create(this, "HD60SUserClientProperties", &client);
    if (ret != kIOReturnSuccess) return ret;
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        client->release();
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

kern_return_t HD60SDriver::CopyFrameRing(IOMemoryDescriptor **memory)
{
    if (!ivars->frameMem) return kIOReturnNotReady;
    ivars->frameMem->retain();
    *memory = ivars->frameMem.get();
    return kIOReturnSuccess;
}

int HD60SDriver::Control(int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
    auto *iv = ivars;
    if (len > kCtrlBytes) return -1;
    if (!in && len) memcpy(iv->ctrlData, buf, len);
    uint16_t done = 0;
    kern_return_t ret = iv->usb->DeviceRequest(this, in ? 0xc0 : 0x40, req, val, idx, len, len ? iv->ctrlBuf.get() : nullptr,
                                               &done, 3000);
    if (ret != kIOReturnSuccess) return -1;
    if (in && done) memcpy(buf, iv->ctrlData, done);
    return done;
}

void HD60SDriver::Frame(const uint8_t *frame, int width, int height, int interlaced)
{
    hd60s_policy_frame(&ivars->policy, width, height, interlaced);
    fr_publish(ivars->frames, frame, width, height, interlaced, ivars->parser.wide, ivars->parser.colour);
}

void HD60SDriver::Audio(const uint8_t *pcm, size_t len)
{
    // ponytail: HDMI vs host clock drift absorbed by ring_pull padding/skipping, as in the userspace feed.
    ring_push(ivars->audio, reinterpret_cast<const int16_t *>(pcm), len / 4);
}

static bool submit(HD60SDriver_IVars *iv, uint32_t i)
{
    auto *ref = static_cast<XferRef *>(iv->xfers[i].action->GetReference());
    ref->index = i;
    ref->generation = iv->generation;
    return iv->pipe->AsyncIO(iv->xfers[i].buf.get(), kXferSize, iv->xfers[i].action.get(), 1000) == kIOReturnSuccess;
}

void HD60SDriver::BringUp()
{
    auto *iv = ivars;
    memset(&iv->dev, 0, sizeof iv->dev);
    iv->dev.usb = {this, ctrlThunk, sleepThunk};
    iv->streamFailed = false;
    iv->generation++;
    if (hd60s_init(&iv->dev) < 0 || hd60s_start(&iv->dev) < 0 || iv->intf->SelectAlternateSetting(4) != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "HD60S dext: bring-up failed (err %d)", iv->dev.err);
        link_step(&iv->link, LINK_UP_FAILED, nowMs(iv));
        TearDown(false);
        return;
    }
    IOUSBHostPipe *pipe = nullptr;
    if (iv->intf->CopyPipe(HD60S_EP_VIDEO, &pipe) != kIOReturnSuccess || !pipe) {
        link_step(&iv->link, LINK_UP_FAILED, nowMs(iv));
        TearDown(false);
        return;
    }
    iv->pipe.reset(pipe, OSNoRetain);
    parser_init(&iv->parser, iv->frameBuf, FR_SLOT_BYTES, frameThunk, audioThunk, this);
    hd60s_policy_begin(&iv->policy, static_cast<long>(nowMs(iv)));
    for (uint32_t i = 0; i < kXfers; i++)
        if (!submit(iv, i)) iv->streamFailed = true;
    link_step(&iv->link, LINK_UP_OK, nowMs(iv));
    os_log(OS_LOG_DEFAULT, "HD60S dext: streaming, MCU %06x", iv->dev.mcu_version);
}

void HD60SDriver::TearDown(bool deviceGone)
{
    auto *iv = ivars;
    iv->generation++;
    if (iv->pipe) {
        // Asynchronous: a synchronous abort would wait for completions queued behind us on this queue.
        iv->pipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
        iv->pipe.reset();
    }
    if (deviceGone) return;
    if (iv->intf) iv->intf->SelectAlternateSetting(0);
    hd60s_stop(&iv->dev);
}

void IMPL(HD60SDriver, BulkDone)
{
    auto *iv = ivars;
    auto *ref = static_cast<XferRef *>(action->GetReference());
    // ponytail: a stale completion is told apart by generation only; fine while retries are 1 s apart.
    if (ref->generation != iv->generation || iv->stopping) return;
    if (status == kIOReturnSuccess || status == kIOReturnTimeout)
        parser_feed(&iv->parser, iv->xfers[ref->index].data, actualByteCount);
    else {
        os_log(OS_LOG_DEFAULT, "HD60S dext: transfer status 0x%x", status);
        iv->streamFailed = true;
        return;
    }
    if (!submit(iv, ref->index)) iv->streamFailed = true;
}

void IMPL(HD60SDriver, PolicyOccurred)
{
    auto *iv = ivars;
    if (iv->stopping) return;
    long now = static_cast<long>(nowMs(iv));
    switch (link_step(&iv->link, iv->streamFailed && iv->link.state == LINK_UP ? LINK_LOST : LINK_TICK, now)) {
    case LINK_BRING_UP: BringUp(); break;
    case LINK_TEAR_DOWN: TearDown(false); break;
    case LINK_NONE: break;
    }
    if (iv->link.state == LINK_UP) hd60s_policy_tick(&iv->dev, &iv->parser, &iv->policy, static_cast<long>(nowMs(iv)));
    fr_status(iv->frames, iv->link.state, iv->parser.frames, iv->parser.discarded, iv->parser.long_total, iv->link.failures);
    fr_device(iv->frames, iv->dev.mcu_version, iv->parser.top_first);
    iv->policyTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + kPolicyMs * iv->msTicks, 0);
}

kern_return_t HD60SDriver::StartDevice(IOUserAudioObjectID id, IOUserAudioStartStopFlags flags)
{
    kern_return_t ret = super::StartDevice(id, flags);
    if (ret != kIOReturnSuccess) return ret;
    ivars->zeroSample = 0;
    ivars->zeroHostTime = mach_absolute_time();
    ivars->device->UpdateCurrentZeroTimestamp(0, ivars->zeroHostTime);
    ivars->zeroTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, ivars->zeroHostTime + ivars->periodTicks, 0);
    ivars->zeroTimer->SetEnable(true);
    return kIOReturnSuccess;
}

kern_return_t HD60SDriver::StopDevice(IOUserAudioObjectID id, IOUserAudioStartStopFlags flags)
{
    ivars->zeroTimer->SetEnable(false);
    return super::StopDevice(id, flags);
}

void IMPL(HD60SDriver, ZeroTimestampOccurred)
{
    ivars->zeroSample += kIOFrames;
    ivars->zeroHostTime += ivars->periodTicks;
    ivars->device->UpdateCurrentZeroTimestamp(ivars->zeroSample, ivars->zeroHostTime);
    ivars->zeroTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, ivars->zeroHostTime + ivars->periodTicks, 0);
}
