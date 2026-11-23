#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <libusb.h>
#include <string.h>

#include "device_libusb.h"
#include "status.h"

static void probe_port(libusb_device *dev, struct hd60s_status *s)
{
    uint8_t ports[8];
    int n = libusb_get_port_numbers(dev, ports, sizeof ports);
    int len = snprintf(s->port_path, sizeof s->port_path, "%d", libusb_get_bus_number(dev));
    for (int i = 0; i < n && len > 0 && (size_t)len < sizeof s->port_path; i++)
        len += snprintf(s->port_path + len, sizeof s->port_path - (size_t)len, "%c%d", i ? '.' : '-', ports[i]);
}

/* UsbExclusiveOwner ("pid N, name") on the IOUSBHostDevice names the process holding the device. */
static void probe_owner(struct hd60s_status *s)
{
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
        return;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        int vid = 0, pid = 0;
        CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("idVendor"), kCFAllocatorDefault, 0);
        CFTypeRef p = IORegistryEntryCreateCFProperty(svc, CFSTR("idProduct"), kCFAllocatorDefault, 0);
        if (v && CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue(v, kCFNumberIntType, &vid);
        if (p && CFGetTypeID(p) == CFNumberGetTypeID())
            CFNumberGetValue(p, kCFNumberIntType, &pid);
        if (v)
            CFRelease(v);
        if (p)
            CFRelease(p);
        if (vid == HD60S_VID && pid == HD60S_PID) {
            CFTypeRef o = IORegistryEntryCreateCFProperty(svc, CFSTR("UsbExclusiveOwner"), kCFAllocatorDefault, 0);
            char text[128], path[PROC_PIDPATHINFO_MAXSIZE] = "";
            int owner_pid;
            if (o && CFGetTypeID(o) == CFStringGetTypeID() &&
                CFStringGetCString(o, text, sizeof text, kCFStringEncodingUTF8) && hd60s_parse_owner_pid(text, &owner_pid)) {
                s->owner_pid = owner_pid;
                proc_pidpath(owner_pid, path, sizeof path);
                const char *base = strrchr(path, '/');
                snprintf(s->owner_name, sizeof s->owner_name, "%s (pid %d)", base ? base + 1 : "unknown", owner_pid);
                s->owner = hd60s_owner_from_path(path);
            } else if (o && CFGetTypeID(o) == CFStringGetTypeID() &&
                       CFStringGetCString(o, text, sizeof text, kCFStringEncodingUTF8)) {
                snprintf(s->owner_name, sizeof s->owner_name, "%s", text);
                s->owner = hd60s_owner_from_service(text);
            }
            if (o)
                CFRelease(o);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
}

void hd60s_probe_usb(struct hd60s_status *s)
{
    libusb_context *ctx;
    if (libusb_init(&ctx) < 0)
        return;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == HD60S_VID && dd.idProduct == HD60S_PID) {
            s->present = 1;
            s->speed = hd60s_speed_from_libusb(libusb_get_device_speed(list[i]));
            probe_port(list[i], s);
            break;
        }
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    if (s->present)
        probe_owner(s);
}
