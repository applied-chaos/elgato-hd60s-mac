#include "DextFeed.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <os/log.h>
#include <stdatomic.h>
#include <time.h>

#include "link.h"

#define FRAME_CLIENT_TYPE 0x68643666 /* 'hd6f', HD60SDriver::NewUserClient */

static struct link session;
static io_connect_t conn;
static mach_vm_address_t addr;
static mach_vm_size_t size;
static IONotificationPortRef port;
static io_object_t interest;
static atomic_int lost;
static atomic_uint connections;
static struct fr_reader reader;

static long now_ms(void) { return (long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000); }

static void on_interest(void *ctx, io_service_t service, natural_t type, void *arg)
{
    (void)ctx, (void)service, (void)arg;
    if (type == kIOMessageServiceIsTerminated)
        atomic_store(&lost, 1);
}

static io_service_t find_driver(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUserService");
    CFStringRef key = CFSTR("IOUserClass"), value = CFSTR("HD60SDriver");
    CFDictionaryRef prop = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&value, 1,
                                              &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), prop);
    CFRelease(prop);
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

static void tear_down(void)
{
    fr_reader_detach(&reader);
    if (interest)
        IOObjectRelease(interest);
    if (port)
        IONotificationPortDestroy(port);
    if (addr)
        IOConnectUnmapMemory64(conn, 0, mach_task_self(), addr);
    if (conn)
        IOServiceClose(conn);
    interest = 0;
    port = NULL;
    addr = 0;
    size = 0;
    conn = 0;
}

static int bring_up(void)
{
    io_service_t driver = find_driver();
    if (!driver)
        return -1;
    atomic_store(&lost, 0);
    kern_return_t kr = IOServiceOpen(driver, mach_task_self(), FRAME_CLIENT_TYPE, &conn);
    if (kr == KERN_SUCCESS)
        kr = IOConnectMapMemory64(conn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere | kIOMapReadOnly);
    if (kr == KERN_SUCCESS) {
        port = IONotificationPortCreate(kIOMainPortDefault);
        IONotificationPortSetDispatchQueue(port, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        kr = IOServiceAddInterestNotification(port, driver, kIOGeneralInterest, on_interest, NULL, &interest);
    }
    IOObjectRelease(driver);
    if (kr != KERN_SUCCESS || !fr_valid((const struct fr_header *)addr, size)) {
        os_log(OS_LOG_DEFAULT, "hd60s: driver extension connect failed 0x%x", kr);
        return -1;
    }
    fr_reader_attach(&reader, (const struct fr_header *)addr, size);
    atomic_fetch_add(&connections, 1);
    os_log(OS_LOG_DEFAULT, "hd60s: connected to driver extension");
    return 0;
}

void dext_feed_maintain(void)
{
    long now = now_ms();
    enum link_action a = link_step(&session, session.state == LINK_UP && atomic_load(&lost) ? LINK_LOST : LINK_TICK, now);
    if (a == LINK_BRING_UP)
        a = link_step(&session, bring_up() == 0 ? LINK_UP_OK : LINK_UP_FAILED, now);
    if (a == LINK_TEAR_DOWN)
        tear_down();
}

int dext_feed_peek(struct fr_frame *info, struct fr_counters *counters, uint32_t *number)
{
    dext_feed_maintain();
    *info = (struct fr_frame){0};
    *number = 0;
    if (!fr_reader_counters(&reader, counters))
        return 0;
    *number = fr_reader_latest(&reader, info);
    return 1;
}

int dext_feed_poll(uint8_t *out, size_t cap, struct fr_frame *info)
{
    return fr_reader_take(&reader, out, cap, info) != 0;
}

unsigned dext_feed_connections(void)
{
    return atomic_load(&connections);
}

struct fr_counters dext_feed_counters(void)
{
    struct fr_counters c;
    fr_reader_counters(&reader, &c);
    return c;
}

void dext_feed_close(void)
{
    tear_down();
    session = (struct link){0};
}

// Opened per call: a held connection keeps a terminated driver's process alive, blocking upgrades and replugs.
int dext_send_settings(const struct hd60s_settings *s)
{
    struct hd60s_settings_wire wire;
    hd60s_settings_to_wire(s, &wire);
    io_service_t driver = find_driver();
    if (!driver)
        return -1;
    io_connect_t control;
    kern_return_t kr = IOServiceOpen(driver, mach_task_self(), FRAME_CLIENT_TYPE, &control);
    IOObjectRelease(driver);
    if (kr != KERN_SUCCESS)
        return -1;
    kr = IOConnectCallStructMethod(control, 0, &wire, sizeof wire, NULL, NULL);
    IOServiceClose(control);
    return kr == KERN_SUCCESS ? 0 : -1;
}
