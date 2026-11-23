#include <DriverKit/DriverKit.h>

#include "HD60SUserClient.h"
#include "HD60SDriver.h"
extern "C" {
#include "device.h"
#include "parser.h"
}

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

kern_return_t HD60SUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments *arguments,
                                              const IOUserClientMethodDispatch *dispatch, OSObject *target, void *reference)
{
    auto *driver = OSDynamicCast(HD60SDriver, GetProvider());
    if (selector != 0 || !driver || !arguments || arguments->scalarInputCount || arguments->scalarOutputCount ||
        arguments->structureOutput || arguments->structureOutputDescriptor || arguments->structureInputDescriptor ||
        !arguments->structureInput || arguments->structureInput->getLength() != sizeof(hd60s_settings_wire))
        return kIOReturnBadArgument;
    hd60s_settings_wire wire;
    memcpy(&wire, arguments->structureInput->getBytesNoCopy(), sizeof wire);
    hd60s_settings settings;
    if (hd60s_settings_from_wire(&wire, sizeof wire, &settings) != 0) return kIOReturnBadArgument;
    driver->SetSettings(&settings);
    return kIOReturnSuccess;
}
