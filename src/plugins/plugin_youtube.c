#include "plugin_youtube.h"

#include "lauxlib.h"
#include "plugin_json.h"
#include "yt_resolver_proc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolution or search itself runs in yt_resolver_proc; this keeps
 * what the plugin side needs until plugin_youtube_poll() delivers it. One
 * slot per function. */
typedef struct {
    /* UI thread only. */
    bool active;
    uint16_t generation;
    lua_State * L;
    int callback_ref;
    int handle_slot;
    const char * name; /* for callback errors */
    atomic_bool cancelled;
    atomic_bool done;

    /* Written by the service's done callback before done: the program's
     * line, on the heap only until it is delivered. */
    char * output;
    size_t output_len;
    /* Fixed strings: set when there is no usable output to decode. */
    const char * fail_class;
    const char * fail_detail;
} plugin_job_t;

static plugin_job_t resolve_job = { .callback_ref = LUA_NOREF };
static plugin_job_t search_job = { .callback_ref = LUA_NOREF };

#ifdef HOST_BUILD
void plugin_youtube_set_test_overrides(const char * path, int ms) {
    yt_resolver_set_test_overrides(path, ms);
}
#endif

/* On the service's worker thread. */
static void take_output(plugin_job_t * j, const char * output, size_t len, const char * fail_class,
                        const char * fail_detail) {
    j->fail_class = fail_class;
    j->fail_detail = fail_detail;
    if (!fail_class) {
        j->output = malloc(len + 1);
        if (j->output) {
            memcpy(j->output, output, len + 1);
            j->output_len = len;
        } else {
            j->fail_class = "internal";
            j->fail_detail = "oom";
        }
    }
    atomic_store(&j->done, true);
}

static void resolve_done(const yt_resolver_result_t * r, void * arg) {
    take_output(arg, r->output, r->output_len, r->fail_class, r->fail_detail);
}

static void search_done(const yt_search_result_t * r, void * arg) {
    take_output(arg, r->output, r->output_len, r->fail_class, r->fail_detail);
}

static int push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

static void reset_job(plugin_job_t * j, int handle_slot, const char * name) {
    memset(j, 0, sizeof(*j));
    atomic_init(&j->cancelled, false);
    atomic_init(&j->done, false);
    j->callback_ref = LUA_NOREF;
    j->handle_slot = handle_slot;
    j->name = name;
}

/* After a start: keeps the callback (at index cb) and returns the handle. */
static int keep_started(lua_State * L, lua_State * owner, plugin_job_t * j, uint16_t generation, int cb) {
    j->generation = generation;
    lua_pushvalue(L, cb);
    j->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    j->L = owner;
    j->active = true;
    lua_pushinteger(L, ((lua_Integer) generation << 8) | j->handle_slot);
    return 1;
}

int plugin_youtube_l_resolve(lua_State * L, lua_State * owner) {
    const char * video_id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!yt_resolver_video_id_valid(video_id)) return push_error(L, "invalid video id");
    if (resolve_job.active || yt_resolver_busy()) return push_error(L, "busy");

    reset_job(&resolve_job, PLUGIN_YOUTUBE_HANDLE_SLOT, "youtube_resolve");
    uint16_t generation = yt_resolver_start(YT_OWNER_PLUGIN, video_id, resolve_done, &resolve_job);
    if (generation == 0) return push_error(L, yt_resolver_busy() ? "busy" : "could not start resolver");
    return keep_started(L, owner, &resolve_job, generation, 2);
}

int plugin_youtube_l_search(lua_State * L, lua_State * owner) {
    size_t query_len = 0, token_len = 0;
    const char * query = luaL_checklstring(L, 1, &query_len);
    const char * token = lua_isnoneornil(L, 2) ? NULL : luaL_checklstring(L, 2, &token_len);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    /* A Lua string may hold a NUL the C checks would stop at. */
    bool query_ok = strlen(query) == query_len && yt_search_query_valid(query);
    bool token_ok = !token || token_len == 0 || (strlen(token) == token_len && yt_search_token_valid(token));
    if (!query_ok || !token_ok) return push_error(L, "bad_input");
    if (search_job.active) return push_error(L, "busy");

    reset_job(&search_job, PLUGIN_YOUTUBE_SEARCH_HANDLE_SLOT, "youtube_search");
    const char * why = "internal";
    uint16_t generation = yt_resolver_start_search(query, token, search_done, &search_job, &why);
    if (generation == 0) return push_error(L, why);
    return keep_started(L, owner, &search_job, generation, 3);
}

bool plugin_youtube_is_handle(int handle) {
    int slot = handle & 0xFF;
    return handle > 0 && (slot == PLUGIN_YOUTUBE_HANDLE_SLOT || slot == PLUGIN_YOUTUBE_SEARCH_HANDLE_SLOT);
}

bool plugin_youtube_cancel(int handle) {
    if (!plugin_youtube_is_handle(handle)) return false;
    plugin_job_t * j = (handle & 0xFF) == PLUGIN_YOUTUBE_SEARCH_HANDLE_SLOT ? &search_job : &resolve_job;
    if (!j->active || (uint16_t) ((unsigned int) handle >> 8) != j->generation) return false;
    if (atomic_exchange(&j->cancelled, true)) return false;
    yt_resolver_cancel(j->generation);
    return true;
}

/* Frees what the job holds once its run has ended. */
static void finish_job(plugin_job_t * j, bool unref) {
    if (unref && j->L && j->callback_ref != LUA_NOREF) luaL_unref(j->L, LUA_REGISTRYINDEX, j->callback_ref);
    j->callback_ref = LUA_NOREF;
    j->L = NULL;
    if (j->output) {
        memset(j->output, 0, j->output_len);
        free(j->output);
    }
    j->output = NULL;
    j->output_len = 0;
    j->active = false;
}

/* Pushes the result table for a finished job onto L. */
static void push_result(lua_State * L, plugin_job_t * j) {
    if (!j->fail_class) {
        /* The program's line, decoded like plugin.json_decode() does:
         * value, nil on success; nil, error (or a raised error) else. */
        int top = lua_gettop(L);
        lua_pushcfunction(L, l_plugin_json_decode);
        lua_pushlstring(L, j->output, j->output_len);
        if (lua_pcall(L, 1, 2, 0) == LUA_OK && lua_istable(L, -2)) {
            lua_pop(L, 1);
            return;
        }
        lua_settop(L, top);
        j->fail_class = "internal";
        j->fail_detail = "resolver_output";
    }
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ok");
    lua_pushstring(L, j->fail_class);
    lua_setfield(L, -2, "class");
    lua_pushstring(L, j->fail_detail);
    lua_setfield(L, -2, "detail");
}

static void poll_job(plugin_job_t * j, int (*call)(lua_State * L, int nargs, int nresults, int errfunc)) {
    if (!j->active || !atomic_load(&j->done)) return;
    /* done is set inside the service's callback; wait for it to finish
     * too, so the callback below can start another run. */
    yt_resolver_wait_ended(j->generation);
    if (!atomic_load(&j->cancelled)) {
        lua_State * L = j->L;
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, j->callback_ref);
        push_result(L, j);
        if (call(L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(L, -1);
            fprintf(stderr, "[plugins] %s callback error: %s\n", j->name, err ? err : "unknown error");
        }
        lua_settop(L, top);
    }
    finish_job(j, true);
}

void plugin_youtube_poll(int (*call)(lua_State * L, int nargs, int nresults, int errfunc)) {
    poll_job(&resolve_job, call);
    poll_job(&search_job, call);
}

static void release_job(plugin_job_t * j, lua_State * owner, bool unref) {
    if (!j->active || (owner && j->L != owner)) return;
    atomic_store(&j->cancelled, true);
    yt_resolver_cancel(j->generation);
    yt_resolver_wait_ended(j->generation);
    finish_job(j, unref);
}

void plugin_youtube_release(lua_State * owner, bool unref) {
    release_job(&resolve_job, owner, unref);
    release_job(&search_job, owner, unref);
}

bool plugin_youtube_busy(void) {
    return resolve_job.active || search_job.active;
}
