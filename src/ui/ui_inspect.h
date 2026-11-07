/* ui_inspect: on request, dumps the visible LVGL object tree to
 * /tmp/hiby-yt-ui/tree-<seq>.tsv for the device runner (hiby-yt,
 * automate-device-tests D5, spec dev/ui-inspect). Format and file
 * protocol: src/core/ui_inspect_format.h.
 *
 * Without /tmp/hiby-yt-ui/request it does nothing but that file's access()
 * check. Local files only: no network route, no log lines. */
#ifndef UI_INSPECT_H
#define UI_INSPECT_H

/* Called on the UI thread, at the top of update_timer_cb (every 500 ms). */
void ui_inspect_poll(void);

#endif
