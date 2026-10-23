#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pipe2() */
#endif
#include "yt_resolver_proc.h"

#include "stream_log.h"
#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
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
    /* Under lock. */
    bool running;
    uint16_t generation;
    yt_owner_t owner;
    int cancel_pipe[2]; /* [1] written by yt_resolver_cancel() */
    atomic_bool cancelled;

    /* Set before the thread starts. */
    char video_id[YT_RESOLVER_VIDEO_ID_LEN + 1];
    bool retry;
    yt_resolver_done_fn done;
    void * done_arg;

    /* Worker only, until done returns. */
    yt_resolver_result_t result;
} resolve_job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended = PTHREAD_COND_INITIALIZER;
static resolve_job_t job = { .cancel_pipe = { -1, -1 } };
/* The last finished resolution, under lock, for the requests waiting on
 * it (a new start replaces job, not this). */
static uint16_t last_generation;
static yt_resolver_result_t last;
static const char * resolver_path = YT_RESOLVER_PATH;
static int timeout_ms = YT_RESOLVER_TIMEOUT_MS;

#ifdef HOST_BUILD
void yt_resolver_set_test_overrides(const char * path, int ms) {
    resolver_path = path ? path : YT_RESOLVER_PATH;
    timeout_ms = ms > 0 ? ms : YT_RESOLVER_TIMEOUT_MS;
}
#endif

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

bool yt_resolver_video_id_valid(const char * id) {
    return yt_video_id_valid(id);
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

/* Runs the resolver and fills j->result; returns the exit description for
 * the log. */
static const char * run_resolver(resolve_job_t * j, uint64_t deadline, char * exit_buf, size_t exit_size) {
    yt_resolver_result_t * res = &j->result;
    if (access(resolver_path, X_OK) != 0) {
        res->fail_class = "internal";
        res->fail_detail = "resolver_missing";
        return "-";
    }
    char * argv[] = { (char *) resolver_path, j->video_id, NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) {
        res->fail_class = "internal";
        res->fail_detail = "spawn_failed";
        return "-";
    }
    (void) setpriority(PRIO_PROCESS, (id_t) pid, RESOLVER_NICE);

    const char * exit_desc = "-";
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
        size_t room = YT_RESOLVER_OUTPUT_MAX - res->output_len;
        size_t take = (size_t) n < room ? (size_t) n : room;
        if (take < (size_t) n) overflow = true;
        memcpy(res->output + res->output_len, buf, take);
        res->output_len += take;
    }

    int status = 0;
    if (!killed && !wait_exit(j, pid, deadline, &status)) killed = true;
    if (killed) {
        subprocess_terminate(pid); /* SIGTERM, then SIGKILL, and reaps */
        exit_desc = "killed";
        if (!atomic_load(&j->cancelled)) {
            res->fail_class = "limit";
            res->fail_detail = "timeout";
        }
    } else if (WIFEXITED(status)) {
        snprintf(exit_buf, exit_size, "%d", WEXITSTATUS(status));
        exit_desc = exit_buf;
    } else if (WIFSIGNALED(status)) {
        snprintf(exit_buf, exit_size, "signal_%d", WTERMSIG(status));
        exit_desc = exit_buf;
        res->fail_class = "internal";
        res->fail_detail = "resolver_crashed";
    }
    close(fd);
    if (!res->fail_class && overflow) {
        res->fail_class = "internal";
        res->fail_detail = "resolver_output";
    }
    res->output[res->output_len] = '\0';
    return exit_desc;
}

/* The class for the log comes from the output line when there is one;
 * only fixed words reach the log. */
static void set_log_class(yt_resolver_result_t * res) {
    char * cls = res->cls;
    size_t size = sizeof(res->cls);
    if (res->cancelled) {
        snprintf(cls, size, "cancelled");
    } else if (res->fail_class) {
        snprintf(cls, size, "%s", res->fail_class);
    } else if (strstr(res->output, "\"ok\":true")) {
        snprintf(cls, size, "ok");
    } else {
        const char * c = strstr(res->output, "\"class\":\"");
        size_t n = 0;
        if (c) {
            c += 9;
            while (n + 1 < size && ((c[n] >= 'a' && c[n] <= 'z') || c[n] == '_')) n++;
            memcpy(cls, c, n);
            cls[n] = '\0';
        }
        if (n == 0) snprintf(cls, size, "internal");
    }
}

static const char * owner_name(yt_owner_t owner) {
    switch (owner) {
        case YT_OWNER_PLUGIN: return "plugin";
        case YT_OWNER_OPEN: return "open";
        case YT_OWNER_PREFETCH: return "prefetch";
        case YT_OWNER_REFRESH: return "refresh";
        default: return "-";
    }
}

static void * resolve_worker(void * arg) {
    resolve_job_t * j = arg;
    uint64_t start = now_ms();
    char exit_buf[24];
    const char * exit_desc = run_resolver(j, start + (uint64_t) timeout_ms, exit_buf, sizeof(exit_buf));
    j->result.cancelled = atomic_load(&j->cancelled);
    set_log_class(&j->result);
    stream_log_event(stream_log_next_sid(), "resolve", "video_id=%s stage=%s cache=miss%s class=%s ms=%llu exit=%s",
                     j->video_id, owner_name(j->owner), j->retry ? " retry=1" : "", j->result.cls,
                     (unsigned long long) (now_ms() - start), exit_desc);
    if (j->done) j->done(&j->result, j->done_arg);

    pthread_mutex_lock(&lock);
    last = j->result;
    last_generation = j->generation;
    close(j->cancel_pipe[0]);
    close(j->cancel_pipe[1]);
    j->cancel_pipe[0] = j->cancel_pipe[1] = -1;
    j->running = false;
    pthread_cond_broadcast(&ended);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* Under lock, with nothing running. */
static uint16_t start_locked(yt_owner_t owner, const char * video_id, bool retry, yt_resolver_done_fn done,
                             void * arg) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return 0;
    uint16_t generation = (uint16_t) (job.generation + 1);
    if (generation == 0) generation = 1;
    memset(&job, 0, sizeof(job));
    job.generation = generation;
    job.owner = owner;
    job.cancel_pipe[0] = pipefd[0];
    job.cancel_pipe[1] = pipefd[1];
    atomic_init(&job.cancelled, false);
    snprintf(job.video_id, sizeof(job.video_id), "%s", video_id);
    job.retry = retry;
    job.done = done;
    job.done_arg = arg;

    pthread_attr_t attr;
    pthread_t thread;
    bool started = pthread_attr_init(&attr) == 0;
    if (started) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&thread, &attr, resolve_worker, &job) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!started) {
        close(pipefd[0]);
        close(pipefd[1]);
        job.cancel_pipe[0] = job.cancel_pipe[1] = -1;
        return 0;
    }
    job.running = true;
    return generation;
}

uint16_t yt_resolver_start(yt_owner_t owner, const char * video_id, yt_resolver_done_fn done, void * arg) {
    if (!yt_resolver_video_id_valid(video_id)) return 0;
    pthread_mutex_lock(&lock);
    uint16_t generation = job.running ? 0 : start_locked(owner, video_id, false, done, arg);
    pthread_mutex_unlock(&lock);
    return generation;
}

/* Under lock. */
static bool cancel_locked(uint16_t generation) {
    if (!job.running || job.generation != generation || atomic_exchange(&job.cancelled, true)) return false;
    char b = 1;
    ssize_t w = write(job.cancel_pipe[1], &b, 1);
    (void) w; /* the worker also checks the flag; a full pipe still wakes it */
    return true;
}

bool yt_resolver_cancel(uint16_t generation) {
    pthread_mutex_lock(&lock);
    bool cancelled = cancel_locked(generation);
    pthread_mutex_unlock(&lock);
    return cancelled;
}

/* Under lock: waits up to YT_RESOLVER_POLL_MS for a resolution to end. */
static void wait_tick_locked(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long) YT_RESOLVER_POLL_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&ended, &lock, &deadline);
}

static void fail(yt_resolver_result_t * out, const char * cls, const char * detail) {
    memset(out, 0, sizeof(*out));
    out->fail_class = cls;
    out->fail_detail = detail;
    snprintf(out->cls, sizeof(out->cls), "%s", cls);
}

yt_resolver_outcome_t yt_resolver_request(yt_owner_t owner, const char * video_id, bool retry,
                                          bool (*cancelled)(void * arg), void * arg, yt_resolver_result_t * out) {
    if (!yt_resolver_video_id_valid(video_id)) {
        fail(out, "bad_input", "video_id");
        return YT_RESOLVER_DONE;
    }
    pthread_mutex_lock(&lock);
    uint16_t waiting_for = 0; /* generation whose result this call takes */
    bool mine = false;        /* this call started it */
    for (;;) {
        if (cancelled && cancelled(arg)) {
            if (mine) cancel_locked(waiting_for);
            pthread_mutex_unlock(&lock);
            return YT_RESOLVER_CANCELLED;
        }
        if (waiting_for) {
            if (job.running && job.generation == waiting_for) {
                wait_tick_locked();
                continue;
            }
            /* Another resolution can only finish on top of it after a
             * full run of its own; then the result is gone. */
            if (last_generation == waiting_for) *out = last;
            else fail(out, "internal", "result_lost");
            pthread_mutex_unlock(&lock);
            return YT_RESOLVER_DONE;
        }
        switch (yt_resolve_decide(job.running ? job.owner : YT_OWNER_NONE, job.video_id, owner, video_id)) {
            case YT_REQ_START:
                waiting_for = start_locked(owner, video_id, retry, NULL, NULL);
                if (!waiting_for) {
                    fail(out, "internal", "spawn_failed");
                    pthread_mutex_unlock(&lock);
                    return YT_RESOLVER_DONE;
                }
                mine = true;
                break;
            case YT_REQ_JOIN:
                waiting_for = job.generation;
                break;
            case YT_REQ_CANCEL_AND_START:
                cancel_locked(job.generation);
                wait_tick_locked();
                break;
            case YT_REQ_WAIT:
                wait_tick_locked();
                break;
            case YT_REQ_BUSY:
            case YT_REQ_SKIP:
            default:
                pthread_mutex_unlock(&lock);
                return YT_RESOLVER_REFUSED;
        }
    }
}

void yt_resolver_wait_ended(uint16_t generation) {
    pthread_mutex_lock(&lock);
    while (job.running && job.generation == generation) pthread_cond_wait(&ended, &lock);
    pthread_mutex_unlock(&lock);
}

bool yt_resolver_busy(void) {
    pthread_mutex_lock(&lock);
    bool running = job.running;
    pthread_mutex_unlock(&lock);
    return running;
}
