#include "wifi_lease_watch.h"

#include <stdio.h>
#include <string.h>

void wifi_lease_watch_reset(wifi_lease_watch_t * w) {
    memset(w, 0, sizeof(*w));
}

bool wifi_lease_watch_observe(wifi_lease_watch_t * w, bool associated, const char * ssid) {
    if (!associated || !ssid || !ssid[0]) return false;
    if (w->known && strcmp(w->ssid, ssid) == 0) return false;
    bool renew = w->known;
    w->known = true;
    snprintf(w->ssid, sizeof(w->ssid), "%s", ssid);
    return renew;
}
