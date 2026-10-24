#ifndef WIFI_LEASE_WATCH_H
#define WIFI_LEASE_WATCH_H

/* When wlan0 needs a new DHCP lease (hiby-yt, spec network/wifi-lease).
 * Pure: fed one `wpa_cli status` sample at a time.
 *
 * Turning Wi-Fi on already gets a lease (wifi_control_enable()), and
 * udhcpc -q exits once it has one, so nothing renews it when
 * wpa_supplicant later joins another network with Wi-Fi still on: the old
 * network's address, route and DNS stay. This tracks the SSID of the last
 * association and asks for a lease when a different one shows up. The
 * first network seen after Wi-Fi is turned on is only noted. Samples
 * without an association change nothing. The SSID, not the BSSID, is
 * compared: access points of one network share its subnet. */

#include <stdbool.h>

#define WIFI_LEASE_WATCH_SSID_LEN 33 /* 32 bytes + NUL, like WIFI_MAX_SSID_LEN */

typedef struct {
    bool known;
    char ssid[WIFI_LEASE_WATCH_SSID_LEN];
} wifi_lease_watch_t;

/* Starts over, as after turning Wi-Fi off: no network noted. */
void wifi_lease_watch_reset(wifi_lease_watch_t * w);

/* One status sample: associated (wpa_state=COMPLETED) and its SSID. True
 * when a new lease should be requested now; the SSID is then noted. */
bool wifi_lease_watch_observe(wifi_lease_watch_t * w, bool associated, const char * ssid);

#endif /* WIFI_LEASE_WATCH_H */
