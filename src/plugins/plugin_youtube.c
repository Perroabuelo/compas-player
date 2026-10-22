#define _GNU_SOURCE /* pipe2() */
#include "plugin_youtube.h"

#include "lauxlib.h"
#include "plugin_json.h"
#include "stream_log.h"
#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Resolver niceness: the audio thread must win the single core. */
#define RESOLVER_NICE 10
/* After the resolver closes stdout it should exit at once; this bounds the
 * wait before it is treated like a hung one. */
#define EXIT_WAIT_POLL_MS 50

typedef struct {
    /* UI thread only. */
    bool active;
    uint16_t generation;
    lua_State * L;
    int callback_ref;
    pthread_t thread;
    int cancel_pipe[2]; /* [1] written by plugin_youtube_cancel() */
    atomic_bool cancelled;
    atomic_bool done;

    /* Set before the thread starts. */
    char video_id[16];

    /* Written by the worker before done. */
    char output[PLUGIN_YOUTUBE_OUTPUT_MAX + 1];
    size_t output_len;
    /* Fixed strings: set when there is no usable output to decode. */
    const char * fail_class;
    const char * fail_detail;
} resolve_job_t;

static resolve_job_t job = { .callback_ref = LUA_NOREF, .cancel_pipe = { -1, -1 } };
static const char * resolver_path = PLUGIN_YOUTUBE_RESOLVER_PATH;
static int timeout_ms = PLUGIN_YOUTUBE_TIMEOUT_MS;

#ifdef HOST_BUILD
void plugin_youtube_set_test_overrides(const char * path, int ms) {
    resolver_path = path ? path : PLUGIN_YOUTUBE_RESOLVER_PATH;
    timeout_ms = ms > 0 ? ms : PLUGIN_YOUTUBE_TIMEOUT_MS;
}
#endif

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static bool video_id_valid(const char * id) {
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= 11) return false;
    }
    return n == 11;
}

/* Waits for pid to exit until deadline. Returns true with *status when it
 * did; false when it is still running at the deadline. */
static bool wait_exit(resolve_job_t * j, pid_t pid, uint64_t deadline, int * status) {
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno != EINTR) {
            *status = 0;
            return true; /* already reaped elsewhere: nothing left to wait for */
        }
        if (now_ms() >= deadline || atomic_load(&j->cancelled)) return false;
        usleep(EXIT_WAIT_POLL_MS * 1000);
    }
}

static void * resolve_worker(void * arg) {
    resolve_job_t * j = arg;
    uint64_t start = now_ms();
    uint64_t deadline = start + (uint64_t) timeout_ms;
    const char * exit_desc = "-";
    char exit_buf[24];

    if (access(resolver_path, X_OK) != 0) {
        j->fail_class = "internal";
        j->fail_detail = "resolver_missing";
        goto out;
    }
    char * argv[] = { (char *) resolver_path, j->video_id, NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) {
        j->fail_class = "internal";
        j->fail_detail = "spawn_failed";
        goto out;
    }
    (void) setpriority(PRIO_PROCESS, (id_t) pid, RESOLVER_NICE);

    bool overflow = false;
    bool killed = false;
    for (;;) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            killed = true;
            break;
        }
        struct pollfd fds[2] = { { .fd = fd, .events = POLLIN }, { .fd = j->cancel_pipe[0], .events = POLLIN } };
        int r = poll(fds, 2, (int) (deadline - now));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (fds[1].revents & POLLIN) || atomic_load(&j->cancelled)) {
            killed = true;
            break;
        }
        if (r == 0) continue; /* re-checks the deadline */
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break; /* EOF (or error): the resolver is done writing */
        size_t room = PLUGIN_YOUTUBE_OUTPUT_MAX - j->output_len;
        size_t take = (size_t) n < room ? (size_t) n : room;
        if (take < (size_t) n) overflow = true;
        memcpy(j->output + j->output_len, buf, take);
        j->output_len += take;
    }

    int status = 0;
    if (!killed && !wait_exit(j, pid, deadline, &status)) killed = true;
    if (killed) {
        subprocess_terminate(pid); /* SIGTERM, then SIGKILL, and reaps */
        exit_desc = "killed";
        if (!atomic_load(&j->cancelled)) {
            j->fail_class = "limit";
            j->fail_detail = "timeout";
        }
    } else if (WIFEXITED(status)) {
        snprintf(exit_buf, sizeof(exit_buf), "%d", WEXITSTATUS(status));
        exit_desc = exit_buf;
    } else if (WIFSIGNALED(status)) {
        snprintf(exit_buf, sizeof(exit_buf), "signal_%d", WTERMSIG(status));
        exit_desc = exit_buf;
        j->fail_class = "internal";
        j->fail_detail = "resolver_crashed";
    }
    close(fd);
    if (!j->fail_class && overflow) {
        j->fail_class = "internal";
        j->fail_detail = "resolver_output";
    }
    j->output[j->output_len] = '\0';

out:;
    /* The class for the log comes from the output line when there is one;
     * only fixed words reach the log. */
    char cls[24] = "-";
    if (atomic_load(&j->cancelled)) {
        snprintf(cls, sizeof(cls), "cancelled");
    } else if (j->fail_class) {
        snprintf(cls, sizeof(cls), "%s", j->fail_class);
    } else if (strstr(j->output, "\"ok\":true")) {
        snprintf(cls, sizeof(cls), "ok");
    } else {
        const char * c = strstr(j->output, "\"class\":\"");
        size_t n = 0;
        if (c) {
            c += 9;
            while (n + 1 < sizeof(cls) && ((c[n] >= 'a' && c[n] <= 'z') || c[n] == '_')) n++;
            memcpy(cls, c, n);
            cls[n] = '\0';
        }
        if (n == 0) snprintf(cls, sizeof(cls), "internal");
    }
    stream_log_event(stream_log_next_sid(), "resolve", "video_id=%s class=%s ms=%llu exit=%s", j->video_id, cls,
                     (unsigned long long) (now_ms() - start), exit_desc);
    atomic_store(&j->done, true);
    return NULL;
}

static int push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

int plugin_youtube_l_resolve(lua_State * L, lua_State * owner) {
    const char * video_id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!video_id_valid(video_id)) return push_error(L, "invalid video id");
    if (job.active) return push_error(L, "busy");

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return push_error(L, "could not start resolver");

    uint16_t generation = (uint16_t) (job.generation + 1);
    if (generation == 0) generation = 1;
    memset(&job, 0, sizeof(job));
    job.generation = generation;
    job.cancel_pipe[0] = pipefd[0];
    job.cancel_pipe[1] = pipefd[1];
    atomic_init(&job.cancelled, false);
    atomic_init(&job.done, false);
    snprintf(job.video_id, sizeof(job.video_id), "%s", video_id);
    lua_pushvalue(L, 2);
    job.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    job.L = owner;

    if (pthread_create(&job.thread, NULL, resolve_worker, &job) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, job.callback_ref);
        close(pipefd[0]);
        close(pipefd[1]);
        job.cancel_pipe[0] = job.cancel_pipe[1] = -1;
        job.callback_ref = LUA_NOREF;
        job.L = NULL;
        return push_error(L, "could not start resolver");
    }
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
    char b = 1;
    ssize_t w = write(job.cancel_pipe[1], &b, 1);
    (void) w; /* the worker also checks the flag; a full pipe still wakes it */
    return true;
}

/* Frees what the job holds once its thread has been joined. */
static void finish_job(bool unref) {
    if (unref && job.L && job.callback_ref != LUA_NOREF) luaL_unref(job.L, LUA_REGISTRYINDEX, job.callback_ref);
    if (job.cancel_pipe[0] >= 0) close(job.cancel_pipe[0]);
    if (job.cancel_pipe[1] >= 0) close(job.cancel_pipe[1]);
    job.cancel_pipe[0] = job.cancel_pipe[1] = -1;
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
    pthread_join(job.thread, NULL);
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
    if (!atomic_exchange(&job.cancelled, true)) {
        char b = 1;
        ssize_t w = write(job.cancel_pipe[1], &b, 1);
        (void) w;
    }
    pthread_join(job.thread, NULL);
    finish_job(unref);
}

bool plugin_youtube_busy(void) {
    return job.active;
}
