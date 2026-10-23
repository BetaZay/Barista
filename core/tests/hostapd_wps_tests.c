#include "utils/includes.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "common/ieee802_11_common.h"
#include "wps/wps.h"
#include "wps/wps_i.h"

static void expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static void check_ie(struct wpabuf *ie)
{
    expect(ie != NULL, "WPS IE allocation failed");
    const u8 *bytes = wpabuf_head(ie);
    size_t length = wpabuf_len(ie);
    expect(length >= 6 && bytes[0] == WLAN_EID_VENDOR_SPECIFIC && bytes[1] == length - 2,
           "invalid WPS IE framing");
#ifdef CONFIG_TENDONIN
    expect(WPA_GET_BE32(bytes + 2) == 0xa4c0e1f4, "GamePad WPS must advertise Nintendo vendor type");
#else
    expect(WPA_GET_BE32(bytes + 2) == 0x0050f204, "standard WPS must retain WFA vendor type");
#endif
    int version = 0;
    int extension = 0;
    for (size_t offset = 6; offset < length;)
    {
        expect(length - offset >= 4, "truncated WPS attribute");
        unsigned type = WPA_GET_BE16(bytes + offset);
        size_t size = WPA_GET_BE16(bytes + offset + 2);
        expect(size <= length - offset - 4, "truncated WPS attribute data");
        if (type == ATTR_VERSION)
        {
            expect(size == 1 && bytes[offset + 4] == 0x10, "incorrect WPS version attribute");
            version = 1;
        }
        if (type == ATTR_VENDOR_EXT) extension = 1;
        offset += 4 + size;
    }
    expect(version, "WPS version missing");
#ifdef CONFIG_TENDONIN
    expect(!extension, "GamePad WPS must not advertise WFA Version2 extension");
#else
    expect(extension, "standard WPS must retain its Version2 extension");
#endif
    struct ieee802_11_elems elems;
    expect(ieee802_11_parse_elems(bytes, length, &elems, 0) == ParseOK && elems.wps_ie,
           "emitted WPS IE must be recognized on receive");
    struct wpabuf *attributes = ieee802_11_vendor_ie_concat(bytes, length, WPS_IE_VENDOR_TYPE);
    expect(attributes && wpabuf_len(attributes) == length - 6,
           "association WPS selector must extract the advertised attributes");
    wpabuf_free(attributes);
    wpabuf_free(ie);
}

int main(void)
{
    check_ie(wps_build_assoc_req_ie(WPS_REQ_ENROLLEE));
    check_ie(wps_build_assoc_resp_ie());

    // Beacon/probe-response advertisements use this encapsulation path.
    struct wpabuf *attributes = wpabuf_alloc(100);
    expect(attributes != NULL, "attribute allocation failed");
    expect(wps_build_version(attributes) == 0 &&
           wps_build_wfa_ext(attributes, 0, NULL, 0, 0) == 0,
           "WPS attribute build failed");
    check_ie(wps_ie_encapsulate(attributes));

    // A captured-format Nintendo IE must be recognized independently of what
    // the builder emits; otherwise a self-consistent but wrong OUI could pass.
    const u8 nintendo[] = {221, 9, 0xa4, 0xc0, 0xe1, 0xf4, 0x10, 0x4a, 0, 1, 0x10};
    struct ieee802_11_elems elems;
    ieee802_11_parse_elems(nintendo, sizeof(nintendo), &elems, 0);
#ifdef CONFIG_TENDONIN
    expect(elems.wps_ie != NULL, "GamePad Nintendo WPS IE must be accepted");
#else
    expect(elems.wps_ie == NULL, "Nintendo compatibility must remain scoped to Tendonin");
#endif
    puts("hostapd WPS wire-format tests: ok");
    return 0;
}
