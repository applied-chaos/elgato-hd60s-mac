#include <DriverKit/DriverKit.h>

#include "HD60SUserClient.h"
#include "HD60SDriver.h"
#include "parser.h"

kern_return_t IMPL(HD60SUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    return ret == kIOReturnSuccess && !OSDynamicCast(HD60SDriver, provider) ? kIOReturnUnsupported : ret;
}

kern_return_t IMPL(HD60SUserClient, Stop)
{
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(HD60SUserClient, CopyClientMemoryForType)
{
    auto *driver = OSDynamicCast(HD60SDriver, GetProvider());
    if (type != 0 || !driver) return kIOReturnBadArgument;
    *options = kIOUserClientMemoryReadOnly;
    return driver->CopyFrameRing(memory);
}

// Selector 0: scalar inputs are an enum hd60s_range, then brightness, contrast, saturation and hue as -128..127 offsets from Elgato's defaults.
kern_return_t HD60SUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments *arguments,
                                              const IOUserClientMethodDispatch *dispatch, OSObject *target, void *reference)
{
    auto *driver = OSDynamicCast(HD60SDriver, GetProvider());
    if (selector != 0 || !driver || !arguments || arguments->scalarInputCount != 5) return kIOReturnBadArgument;
    uint64_t range = arguments->scalarInput[0];
    if (range > RANGE_EXPANDED) return kIOReturnBadArgument;
    int picture[4];
    for (int i = 0; i < 4; i++) {
        int64_t v = static_cast<int64_t>(arguments->scalarInput[i + 1]);
        if (v < -128 || v > 127) return kIOReturnBadArgument;
        picture[i] = static_cast<int>(v);
    }
    driver->SetSettings(static_cast<int>(range), picture[0], picture[1], picture[2], picture[3]);
    return kIOReturnSuccess;
}
