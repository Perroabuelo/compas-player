#ifndef PLUGIN_YOUTUBE_H
#define PLUGIN_YOUTUBE_H

/* plugin.youtube_resolve(video_id, callback) (hiby-yt). Runs the local
 * YouTube resolver, a separate program installed at a fixed path, through
 * yt_resolver_proc, and hands its one-line JSON result to callback(result)
 * on the UI thread from plugin_manager_poll().
 *
 * One resolution at a time in the whole player: while the YouTube queue
 * or another plugin call is resolving, this returns nil, "busy". The only
 * variable argument is the video id, checked against [A-Za-z0-9_-]{11}
 * before anything starts, so a plugin from the SD card cannot run any
 * other program. A resolver that has not finished after
 * PLUGIN_YOUTUBE_TIMEOUT_MS is killed and reaped, and the callback gets
 * {ok = false, class = "limit", detail = "timeout"}.
 *
 * Handles share plugin.cancel() with plugin.http_request(): their low byte
 * is PLUGIN_YOUTUBE_HANDLE_SLOT, never an HTTP slot. */

#include "lua.h"
#include "yt_resolver_proc.h"

#include <stdbool.h>

#define PLUGIN_YOUTUBE_RESOLVER_PATH YT_RESOLVER_PATH
#define PLUGIN_YOUTUBE_TIMEOUT_MS YT_RESOLVER_TIMEOUT_MS
#define PLUGIN_YOUTUBE_OUTPUT_MAX YT_RESOLVER_OUTPUT_MAX
#define PLUGIN_YOUTUBE_HANDLE_SLOT 0xFF

/* Lua binding body: plugin.youtube_resolve(video_id, callback) -> handle |
 * nil, error. owner is the plugin's main Lua state (callbacks run there). */
int plugin_youtube_l_resolve(lua_State * L, lua_State * owner);

/* plugin.cancel() for a youtube handle: stops the resolver (the callback is
 * not called). Returns whether it was still running. */
bool plugin_youtube_cancel(int handle);

/* True when handle is a plugin.youtube_resolve() handle. */
bool plugin_youtube_is_handle(int handle);

/* From plugin_manager_poll(): delivers a finished resolution through call
 * (plugin_manager.c's plugin_call). */
void plugin_youtube_poll(int (*call)(lua_State * L, int nargs, int nresults, int errfunc));

/* Stops and reaps a plugin resolution owned by owner (any owner when
 * NULL), without calling back. unref is false when owner's Lua state is
 * already unusable (an aborted load). */
void plugin_youtube_release(lua_State * owner, bool unref);

/* True while a plugin resolution is running or waiting to be delivered. */
bool plugin_youtube_busy(void);

#ifdef HOST_BUILD
/* PC tests only: another resolver program and time limit. */
void plugin_youtube_set_test_overrides(const char * resolver_path, int timeout_ms);
#endif

#endif /* PLUGIN_YOUTUBE_H */
