#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include <stdbool.h>
#include <stddef.h>

/* Blocking worker-only API: returns true if connected to an access point,
 * checking `wpa_cli -i wlan0
 * status` for "wpa_state=COMPLETED". On success, populates
 * *out_signal_level with a 0 (weakest) - 3 (strongest) bucket derived from
 * `wpa_cli -i wlan0 signal_poll`'s RSSI reading; these are conventional
 * RSSI buckets, not values extracted from the stock firmware. Returns false
 * (leaving *out_signal_level untouched) if not connected, or if
 * wpa_cli/wpa_supplicant aren't available at all (wifi off, or no wlan0
 * supplicant instance -- always the case on the host simulator); callers
 * should show the disconnected icon in every such case. Call from a worker
 * thread, never directly from the UI thread. */
bool wifi_get_status(int * out_signal_level);

/* hiby-yt: like wifi_get_status(), and also copies the associated network's
 * SSID into ssid (empty when not associated), for
 * wifi_control_check_lease(). Same worker-only rule. */
bool wifi_get_status_ssid(int * out_signal_level, char * ssid, size_t ssid_size);

#endif /* WIFI_STATUS_H */
