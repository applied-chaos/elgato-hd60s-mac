#include <DriverKit/DriverKit.h>

#include "HD60SUserClient.h"
#include "HD60SDriver.h"

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
