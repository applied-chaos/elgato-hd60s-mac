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

// Selector 0: scalar input 0 is an enum hd60s_range.
kern_return_t HD60SUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments *arguments,
                                              const IOUserClientMethodDispatch *dispatch, OSObject *target, void *reference)
{
    auto *driver = OSDynamicCast(HD60SDriver, GetProvider());
    if (selector != 0 || !driver || !arguments || arguments->scalarInputCount != 1) return kIOReturnBadArgument;
    uint64_t range = arguments->scalarInput[0];
    if (range > RANGE_EXPANDED) return kIOReturnBadArgument;
    driver->SetRange(static_cast<int>(range));
    return kIOReturnSuccess;
}
