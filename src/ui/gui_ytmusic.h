#ifndef GUI_YTMUSIC_H
#define GUI_YTMUSIC_H

/* The YouTube Music screen (hiby-yt, spec youtube/music-screen, design D1
 * of youtube-music-screen): Home, Explore and Library tabs, search with
 * two-line result rows, queue actions and a mini player. Built once by
 * gui_ytmusic_init() and kept with its state until gui_ytmusic_teardown()
 * (an in-process UI reload), like the Subsonic screens. */

#include "lvgl/lvgl.h"

#include <stdbool.h>

void gui_ytmusic_init(void);
/* Cancels a search in flight (the program is killed and reaped), frees the
 * results and deletes the screen, so gui_ytmusic_init() can start again
 * from a clean slate. */
void gui_ytmusic_teardown(void);
/* Pushes the screen, in the view it had. */
void gui_ytmusic_open(void);
/* Home tile callback. */
void ytmusic_tile_cb(lv_event_t * e);
/* From the UI loop: takes a finished search. */
void poll_ytmusic(void);

#endif /* GUI_YTMUSIC_H */
