#include "plugin_youtube.h"

#include "lauxlib.h"
#include "plugin_json.h"
#include "yt_resolver_proc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The resolution itself runs in yt_resolver_proc; this keeps what the
 * plugin side needs until plugin_youtube_poll() delivers it. */
typedef struct {
    /* UI thread only. */
    bool active;
    uint16_t generation;
    lua_State * L;
    int callback_ref;
    atomic_bool cancelled;
    atomic_bool done;

    /* Written by the resolver's done callback before done. */
    char output[PLUGIN_YOUTUBE_OUTPUT_MAX + 1];
    size_t output_len;
    /* Fixed strings: set when there is no usable output to decode. */
    const char * fail_class;
    const char * fail_detail;
} plugin_job_t;

static plugin_job_t job = { .callback_ref = LUA_NOREF };

#ifdef HOST_BUILD
void plugin_youtube_set_test_overrides(const char * path, int ms) {
    yt_resolver_set_test_overrides(path, ms);
}
#endif

/* On the resolver's worker thread. */
static void resolve_done(const yt_resolver_result_t * r, void * arg) {
    plugin_job_t * j = arg;
    memcpy(j->output, r->output, r->output_len + 1);
    j->output_len = r->output_len;
    j->fail_class = r->fail_class;
    j->fail_detail = r->fail_detail;
    atomic_store(&j->done, true);
}

static int push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

int plugin_youtube_l_resolve(lua_State * L, lua_State * owner) {
    const char * video_id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!yt_resolver_video_id_valid(video_id)) return push_error(L, "invalid video id");
    if (job.active || yt_resolver_busy()) return push_error(L, "busy");

    memset(&job, 0, sizeof(job));
    atomic_init(&job.cancelled, false);
    atomic_init(&job.done, false);
    job.callback_ref = LUA_NOREF;
    uint16_t generation = yt_resolver_start(YT_OWNER_PLUGIN, video_id, resolve_done, &job);
    if (generation == 0) return push_error(L, yt_resolver_busy() ? "busy" : "could not start resolver");
    job.generation = generation;
    lua_pushvalue(L, 2);
    job.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    job.L = owner;
    job.active = true;
    lua_pushinteger(L, ((lua_Integer) generation << 8) | PLUGIN_YOUTUBE_HANDLE_SLOT);
    return 1;
}

bool plugin_youtube_is_handle(int handle) {
    return handle > 0 && (handle & 0xFF) == PLUGIN_YOUTUBE_HANDLE_SLOT;
}

bool plugin_youtube_cancel(int handle) {
    if (!plugin_youtube_is_handle(handle) || !job.active) return false;
    if ((uint16_t) ((unsigned int) handle >> 8) != job.generation) return false;
    if (atomic_exchange(&job.cancelled, true)) return false;
    yt_resolver_cancel(job.generation);
    return true;
}

/* Frees what the job holds once its resolution has ended. */
static void finish_job(bool unref) {
    if (unref && job.L && job.callback_ref != LUA_NOREF) luaL_unref(job.L, LUA_REGISTRYINDEX, job.callback_ref);
    job.callback_ref = LUA_NOREF;
    job.L = NULL;
    job.active = false;
}

/* Pushes the result table for a finished job onto L. */
static void push_result(lua_State * L) {
    if (!job.fail_class) {
        /* The resolver's line, decoded like plugin.json_decode() does:
         * value, nil on success; nil, error (or a raised error) else. */
        int top = lua_gettop(L);
        lua_pushcfunction(L, l_plugin_json_decode);
        lua_pushlstring(L, job.output, job.output_len);
        if (lua_pcall(L, 1, 2, 0) == LUA_OK && lua_istable(L, -2)) {
            lua_pop(L, 1);
            return;
        }
        lua_settop(L, top);
        job.fail_class = "internal";
        job.fail_detail = "resolver_output";
    }
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ok");
    lua_pushstring(L, job.fail_class);
    lua_setfield(L, -2, "class");
    lua_pushstring(L, job.fail_detail);
    lua_setfield(L, -2, "detail");
}

void plugin_youtube_poll(int (*call)(lua_State * L, int nargs, int nresults, int errfunc)) {
    if (!job.active || !atomic_load(&job.done)) return;
    /* done is set inside the resolver's callback; wait for it to finish
     * too, so the callback below can start another resolution. */
    yt_resolver_wait_ended(job.generation);
    if (!atomic_load(&job.cancelled)) {
        lua_State * L = job.L;
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, job.callback_ref);
        push_result(L);
        if (call(L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(L, -1);
            fprintf(stderr, "[plugins] youtube_resolve callback error: %s\n", err ? err : "unknown error");
        }
        lua_settop(L, top);
    }
    finish_job(true);
}

void plugin_youtube_release(lua_State * owner, bool unref) {
    if (!job.active || (owner && job.L != owner)) return;
    atomic_store(&job.cancelled, true);
    yt_resolver_cancel(job.generation);
    yt_resolver_wait_ended(job.generation);
    finish_job(unref);
}

bool plugin_youtube_busy(void) {
    return job.active;
}
