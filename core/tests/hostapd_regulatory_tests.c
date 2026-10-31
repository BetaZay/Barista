#include "utils/includes.h"
#include "utils/common.h"
#include "ap/hostapd.h"
#include "ap/ap_drv_ops.h"
#include "ap/regulatory_prescan.h"

static int scan_calls;
static int scan_result;
static int sleep_calls;
static struct hostapd_data *expected_hapd;

static void expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

int hostapd_driver_scan(struct hostapd_data *hapd,
                        struct wpa_driver_scan_params *params)
{
    struct wpa_driver_scan_params passive = {0};
    expect(hapd == expected_hapd, "scan must use the selected AP interface");
    expect(memcmp(params, &passive, sizeof(passive)) == 0,
           "recovery must request a passive scan without overriding channel restrictions");
    scan_calls++;
    return scan_result;
}

void os_sleep(os_time_t sec, os_time_t usec)
{
    expect(sec == 6 && usec == 0, "recovery wait must stay bounded to six seconds");
    sleep_calls++;
}

void wpa_printf(int level, const char *fmt, ...)
{
    (void) level;
    (void) fmt;
}

int main(void)
{
    struct hostapd_data hapd = {0};
    struct hostapd_data *bss[] = {&hapd};
    struct hostapd_iface iface = {0};
    iface.bss = bss;
    expected_hapd = &hapd;

    int attempted = 0;
    expect(hostapd_regulatory_prescan(&iface, &attempted),
           "accepted scan must allow channel selection to retry");
    expect(attempted && scan_calls == 1 && sleep_calls == 1,
           "first failure must trigger one bounded scan wait");
    expect(!hostapd_regulatory_prescan(&iface, &attempted),
           "continued NO-IR must not cause unbounded retries");
    expect(scan_calls == 1 && sleep_calls == 1, "retry must not scan or sleep again");

    attempted = 0;
    scan_result = -1;
    expect(!hostapd_regulatory_prescan(&iface, &attempted),
           "rejected scan must retain the original channel failure");
    expect(attempted && scan_calls == 2 && sleep_calls == 1,
           "rejected scan must not incur a wait");
    expect(!hostapd_regulatory_prescan(&iface, &attempted) && scan_calls == 2,
           "driver scan rejection must also consume the single attempt");

    attempted = 0;
    bss[0] = NULL;
    expect(!hostapd_regulatory_prescan(&iface, &attempted) && scan_calls == 2,
           "missing BSS must not call the driver");
    puts("hostapd regulatory recovery tests: ok");
    return 0;
}
