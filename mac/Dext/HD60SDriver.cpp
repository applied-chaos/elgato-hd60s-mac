#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "HD60SDriver.h"
#include "Tone.h"

static constexpr double kSampleRate = 48000.0;
static constexpr uint32_t kChannels = 2;
static constexpr uint32_t kRingFrames = 16384;
static constexpr uint32_t kBytesPerFrame = kChannels * sizeof(float);
// Prototype name differs from the HAL plug-in's "Elgato HD60 S" so both can coexist.
static constexpr const char *kDeviceName = "Elgato HD60 S (DriverKit)";

struct HD60SDriver_IVars {
    OSSharedPtr<IODispatchQueue> queue;
    OSSharedPtr<IOUserAudioDevice> device;
    OSSharedPtr<IOUserAudioStream> stream;
    OSSharedPtr<IOBufferMemoryDescriptor> ring;
    OSSharedPtr<IOTimerDispatchSource> timer;
    OSSharedPtr<OSAction> timerAction;
    IOMemoryMap *ringMap = nullptr;
    uint64_t periodTicks = 0;
    uint64_t zeroHostTime = 0;
    uint64_t zeroSample = 0;
};

bool HD60SDriver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(HD60SDriver_IVars, 1);
    return ivars != nullptr;
}

void HD60SDriver::free()
{
    IOSafeDeleteNULL(ivars, HD60SDriver_IVars, 1);
    super::free();
}

kern_return_t IMPL(HD60SDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ivars->queue = GetWorkQueue();
    auto name = OSSharedPtr(OSString::withCString(kDeviceName), OSNoRetain);
    SetName(name.get());

    auto uid = OSSharedPtr(OSString::withCString("net.mintopia.hd60s.dext.device"), OSNoRetain);
    auto model = OSSharedPtr(OSString::withCString("HD60 S"), OSNoRetain);
    auto maker = OSSharedPtr(OSString::withCString("Elgato"), OSNoRetain);
    ivars->device = IOUserAudioDevice::Create(this, false, uid.get(), model.get(), maker.get(), kRingFrames);
    if (!ivars->device) return kIOReturnNoMemory;
    ivars->device->SetName(name.get());
    ivars->device->SetSampleRate(kSampleRate);
    ivars->device->SetTransportType(IOUserAudioTransportType::USB);
    ivars->device->SetCanBeDefaultInputDevice(false);

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kRingFrames * kBytesPerFrame, 0, ivars->ring.attach());
    if (ret != kIOReturnSuccess) return ret;
    ivars->ring->SetLength(kRingFrames * kBytesPerFrame);
    ret = ivars->ring->CreateMapping(0, 0, 0, 0, 0, &ivars->ringMap);
    if (ret != kIOReturnSuccess) return ret;

    ivars->stream = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Input, ivars->ring.get());
    if (!ivars->stream) return kIOReturnNoMemory;
    IOUserAudioStreamBasicDescription format = {};
    format.mSampleRate = kSampleRate;
    format.mFormatID = IOUserAudioFormatID::LinearPCM;
    format.mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | IOUserAudioFormatFlags::FormatFlagIsPacked);
    format.mBytesPerPacket = kBytesPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    ivars->stream->SetAvailableStreamFormats(&format, 1);
    ivars->stream->SetCurrentStreamFormat(&format);
    ret = ivars->device->AddStream(ivars->stream.get());
    if (ret != kIOReturnSuccess) return ret;

    ivars->device->SetIOOperationHandler(^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation op,
                                                        uint32_t frames, uint64_t sampleTime, uint64_t) {
        if (op != IOUserAudioIOOperationBeginRead || !ivars->ringMap) return kIOReturnSuccess;
        auto *base = reinterpret_cast<float *>(ivars->ringMap->GetAddress());
        uint32_t at = sampleTime % kRingFrames;
        uint32_t first = frames < kRingFrames - at ? frames : kRingFrames - at;
        tone_fill(base + at * kChannels, sampleTime, first, kSampleRate);
        if (first < frames) tone_fill(base, sampleTime + first, frames - first, kSampleRate);
        return kIOReturnSuccess;
    });

    ret = AddObject(ivars->device.get());
    if (ret != kIOReturnSuccess) return ret;

    ret = IOTimerDispatchSource::Create(ivars->queue.get(), ivars->timer.attach());
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionZeroTimestampOccurred(0, ivars->timerAction.attach());
    if (ret != kIOReturnSuccess) return ret;
    ivars->timer->SetHandler(ivars->timerAction.get());

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double periodNs = 1e9 * kRingFrames / kSampleRate;
    ivars->periodTicks = static_cast<uint64_t>(periodNs * tb.denom / tb.numer);

    ret = RegisterService();
    os_log(OS_LOG_DEFAULT, "HD60S dext started: %d", ret);
    return ret;
}

kern_return_t IMPL(HD60SDriver, Stop)
{
    if (ivars->timer) ivars->timer->Cancel(nullptr);
    if (ivars->device) RemoveObject(ivars->device.get());
    if (ivars->ringMap) { ivars->ringMap->release(); ivars->ringMap = nullptr; }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t HD60SDriver::StartDevice(IOUserAudioObjectID id, IOUserAudioStartStopFlags flags)
{
    kern_return_t ret = super::StartDevice(id, flags);
    if (ret != kIOReturnSuccess) return ret;
    ivars->zeroSample = 0;
    ivars->zeroHostTime = mach_absolute_time();
    ivars->device->UpdateCurrentZeroTimestamp(0, ivars->zeroHostTime);
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, ivars->zeroHostTime + ivars->periodTicks, 0);
    ivars->timer->SetEnable(true);
    return kIOReturnSuccess;
}

kern_return_t HD60SDriver::StopDevice(IOUserAudioObjectID id, IOUserAudioStartStopFlags flags)
{
    ivars->timer->SetEnable(false);
    return super::StopDevice(id, flags);
}

void IMPL(HD60SDriver, ZeroTimestampOccurred)
{
    ivars->zeroSample += kRingFrames;
    ivars->zeroHostTime += ivars->periodTicks;
    ivars->device->UpdateCurrentZeroTimestamp(ivars->zeroSample, ivars->zeroHostTime);
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, ivars->zeroHostTime + ivars->periodTicks, 0);
}
