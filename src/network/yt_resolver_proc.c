#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pipe2() */
#endif
#include "yt_resolver_proc.h"

#include "stream_log.h"
#include "subprocess.h"
#include "yt_job_admit.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
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
    bool preempted;           /* cancelled by preempted_by, which goes first */
    yt_owner_t preempted_by;

    /* Set before the thread starts. */
    char video_id[YT_RESOLVER_VIDEO_ID_LEN + 1];
    bool retry;
    yt_resolver_done_fn done;
    void * done_arg;
    /* A search or a content page (a line job): its request line (heap,
     * wiped and freed by the worker), and the page's kind. */
    char * request;
    size_t request_len;
    yt_browse_kind_t kind;
    yt_search_done_fn line_done;

    /* Worker only, until done returns. */
    yt_resolver_result_t result;
} resolve_job_t;

/* A search or a content page waiting for the prefetch or radio it
 * cancelled to be reaped (under lock). It counts as running for every
 * other request. */
typedef struct {
    bool active;
    bool cancelled;
    uint16_t generation;
    yt_owner_t owner;
    yt_browse_kind_t kind;
    char * request;
    size_t request_len;
    yt_search_done_fn done;
    void * done_arg;
} pending_line_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended = PTHREAD_COND_INITIALIZER;
static resolve_job_t job = { .cancel_pipe = { -1, -1 } };
static pending_line_t pending;
static uint16_t generations;
/* The last finished resolution, under lock, for the requests waiting on
 * it (a new start replaces job, not this). */
static uint16_t last_generation;
static yt_resolver_result_t last;
static const char * resolver_path = YT_RESOLVER_PATH;
static int timeout_ms = YT_RESOLVER_TIMEOUT_MS;
static int search_timeout_ms = YT_SEARCH_PROC_TIMEOUT_MS;
static int browse_timeout_ms = YT_BROWSE_PROC_TIMEOUT_MS;

#ifdef HOST_BUILD
void yt_resolver_set_test_overrides(const char * path, int ms) {
    resolver_path = path ? path : YT_RESOLVER_PATH;
    timeout_ms = ms > 0 ? ms : YT_RESOLVER_TIMEOUT_MS;
}

void yt_resolver_set_search_test_timeout(int ms) {
    search_timeout_ms = ms > 0 ? ms : YT_SEARCH_PROC_TIMEOUT_MS;
}

void yt_resolver_set_browse_test_timeout(int ms) {
    browse_timeout_ms = ms > 0 ? ms : YT_BROWSE_PROC_TIMEOUT_MS;
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

static void wipe_free(char * p, size_t n) {
    if (!p) return;
    memset(p, 0, n);
    free(p);
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

/* Where run_child() leaves what the child printed and why it failed. */
typedef struct {
    char * out;     /* out_max + 1 bytes */
    size_t out_max;
    size_t out_len;
    const char * fail_class;
    const char * fail_detail;
} child_io_t;

/* Writes all of input to fd. False when the child stopped reading. */
static bool write_all(int fd, const char * input, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, input + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        off += (size_t) w;
    }
    return true;
}

/* Runs argv, with input on its stdin when not NULL (stdin is closed right
 * after it), reads its stdout into io until it closes, and reaps it; kills
 * it at the deadline or on cancel. Returns the exit description for the
 * log. */
static const char * run_child(resolve_job_t * j, char * const argv[], const char * input, size_t input_len,
                              uint64_t deadline, child_io_t * io, char * exit_buf, size_t exit_size) {
    if (access(resolver_path, X_OK) != 0) {
        io->fail_class = "internal";
        io->fail_detail = "resolver_missing";
        return "-";
    }
    pid_t pid;
    int fd, in_fd = -1;
    bool spawned = input ? subprocess_popen2(argv, &pid, &in_fd, &fd) : subprocess_popen(argv, &pid, &fd);
    if (!spawned) {
        io->fail_class = "internal";
        io->fail_detail = "spawn_failed";
        return "-";
    }
    (void) setpriority(PRIO_PROCESS, (id_t) pid, RESOLVER_NICE);

    const char * exit_desc = "-";
    bool overflow = false;
    bool killed = false;
    if (input) {
        bool written = write_all(in_fd, input, input_len);
        close(in_fd); /* end of input */
        if (!written) {
            subprocess_terminate(pid);
            close(fd);
            io->fail_class = "internal";
            io->fail_detail = "stdin_write";
            io->out[0] = '\0';
            return "killed";
        }
    }
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
        size_t room = io->out_max - io->out_len;
        size_t take = (size_t) n < room ? (size_t) n : room;
        if (take < (size_t) n) overflow = true;
        memcpy(io->out + io->out_len, buf, take);
        io->out_len += take;
    }

    int status = 0;
    if (!killed && !wait_exit(j, pid, deadline, &status)) killed = true;
    if (killed) {
        subprocess_terminate(pid); /* SIGTERM, then SIGKILL, and reaps */
        exit_desc = "killed";
        if (!atomic_load(&j->cancelled)) {
            io->fail_class = "limit";
            io->fail_detail = "timeout";
        }
    } else if (WIFEXITED(status)) {
        snprintf(exit_buf, exit_size, "%d", WEXITSTATUS(status));
        exit_desc = exit_buf;
    } else if (WIFSIGNALED(status)) {
        snprintf(exit_buf, exit_size, "signal_%d", WTERMSIG(status));
        exit_desc = exit_buf;
        io->fail_class = "internal";
        io->fail_detail = "resolver_crashed";
    }
    close(fd);
    if (!io->fail_class && overflow) {
        io->fail_class = "internal";
        io->fail_detail = "resolver_output";
    }
    io->out[io->out_len] = '\0';
    return exit_desc;
}

/* The class for the log comes from the output line when there is one;
 * only fixed words reach the log. */
static void set_log_class(char * cls, size_t size, bool cancelled, const char * fail_class, const char * output) {
    if (cancelled) {
        snprintf(cls, size, "cancelled");
    } else if (fail_class) {
        snprintf(cls, size, "%s", fail_class);
    } else if (strstr(output, "\"ok\":true")) {
        snprintf(cls, size, "ok");
    } else {
        const char * c = strstr(output, "\"class\":\"");
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
        case YT_OWNER_SEARCH: return "search";
        case YT_OWNER_BROWSE: return "browse";
        case YT_OWNER_RADIO: return "radio";
        default: return "-";
    }
}

/* The event=search or event=browse line of a line job. */
static void log_line_job(yt_owner_t owner, yt_browse_kind_t kind, const char * cls, unsigned long long ms,
                         const char * exit_desc) {
    if (owner == YT_OWNER_SEARCH) {
        stream_log_event(stream_log_next_sid(), "search", "class=%s ms=%llu exit=%s", cls, ms, exit_desc);
    } else {
        const char * name = yt_browse_kind_name(kind);
        stream_log_event(stream_log_next_sid(), "browse", "kind=%s class=%s ms=%llu exit=%s", name ? name : "-", cls,
                         ms, exit_desc);
    }
}

static uint16_t start_line_locked(uint16_t generation, yt_owner_t owner, yt_browse_kind_t kind, char * request,
                                  size_t request_len, yt_search_done_fn done, void * arg);

/* Under lock, after a job ended: starts the search or page that was
 * waiting for it. One cancelled while it waited never starts: its done call
 * runs here, before the waiters of its generation wake up, so it cannot
 * land on a later job of the caller. done callbacks do not take this
 * lock. */
static void start_pending_locked(void) {
    if (!pending.active) return;
    pending_line_t p = pending;
    memset(&pending, 0, sizeof(pending));
    if (!p.cancelled &&
        start_line_locked(p.generation, p.owner, p.kind, p.request, p.request_len, p.done, p.done_arg))
        return;
    wipe_free(p.request, p.request_len);
    yt_search_result_t res = { .cancelled = p.cancelled, .output = "", .output_len = 0 };
    if (!p.cancelled) {
        res.fail_class = "internal";
        res.fail_detail = "spawn_failed";
    }
    set_log_class(res.cls, sizeof(res.cls), res.cancelled, res.fail_class, "");
    log_line_job(p.owner, p.kind, res.cls, 0, "-");
    if (p.done) p.done(&res, p.done_arg);
}

/* Under lock: the job is over; starts what waited and wakes the waiters. */
static void end_job_locked(resolve_job_t * j) {
    close(j->cancel_pipe[0]);
    close(j->cancel_pipe[1]);
    j->cancel_pipe[0] = j->cancel_pipe[1] = -1;
    j->running = false;
    start_pending_locked();
    pthread_cond_broadcast(&ended);
}

static void * resolve_worker(void * arg) {
    resolve_job_t * j = arg;
    uint64_t start = now_ms();
    char exit_buf[24];
    char * argv[] = { (char *) resolver_path, j->video_id, NULL };
    yt_resolver_result_t * res = &j->result;
    child_io_t io = { .out = res->output, .out_max = YT_RESOLVER_OUTPUT_MAX };
    const char * exit_desc = run_child(j, argv, NULL, 0, start + (uint64_t) timeout_ms, &io, exit_buf, sizeof(exit_buf));
    res->output_len = io.out_len;
    res->fail_class = io.fail_class;
    res->fail_detail = io.fail_detail;
    res->cancelled = atomic_load(&j->cancelled);
    pthread_mutex_lock(&lock);
    res->preempted = res->cancelled && j->preempted;
    pthread_mutex_unlock(&lock);
    set_log_class(res->cls, sizeof(res->cls), res->cancelled, res->fail_class, res->output);
    stream_log_event(stream_log_next_sid(), "resolve", "video_id=%s stage=%s cache=miss%s class=%s%s%s ms=%llu exit=%s",
                     j->video_id, owner_name(j->owner), j->retry ? " retry=1" : "", res->cls,
                     res->preempted ? " reason=" : "", res->preempted ? owner_name(j->preempted_by) : "",
                     (unsigned long long) (now_ms() - start), exit_desc);
    if (j->done) j->done(res, j->done_arg);

    pthread_mutex_lock(&lock);
    last = *res;
    last_generation = j->generation;
    end_job_locked(j);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* A search (--search) or a content page (--browse): the request on stdin,
 * one line back in a buffer taken here and freed after done. */
static void * line_worker(void * arg) {
    resolve_job_t * j = arg;
    uint64_t start = now_ms();
    char exit_buf[24];
    const char * exit_desc = "-";
    bool search = j->owner == YT_OWNER_SEARCH;
    size_t out_max = search ? YT_SEARCH_PROC_OUTPUT_MAX : YT_BROWSE_PROC_OUTPUT_MAX;
    int limit_ms = search ? search_timeout_ms : browse_timeout_ms;
    yt_search_result_t res = { 0 };
    child_io_t io = { .out = malloc(out_max + 1), .out_max = out_max };
    if (io.out) {
        char * argv[] = { (char *) resolver_path, (char *) (search ? "--search" : "--browse"), NULL };
        exit_desc = run_child(j, argv, j->request, j->request_len, start + (uint64_t) limit_ms, &io, exit_buf,
                              sizeof(exit_buf));
    } else {
        io.fail_class = "internal";
        io.fail_detail = "oom";
    }
    /* The query, the ids and the token go no further than the child's
     * stdin. */
    wipe_free(j->request, j->request_len);
    j->request = NULL;
    res.output = io.out ? io.out : "";
    res.output_len = io.out_len;
    res.fail_class = io.fail_class;
    res.fail_detail = io.fail_detail;
    res.cancelled = atomic_load(&j->cancelled);
    pthread_mutex_lock(&lock);
    res.preempted = res.cancelled && j->preempted;
    pthread_mutex_unlock(&lock);
    set_log_class(res.cls, sizeof(res.cls), res.cancelled, res.fail_class, res.output);
    log_line_job(j->owner, j->kind, res.cls, (unsigned long long) (now_ms() - start), exit_desc);
    if (j->line_done) j->line_done(&res, j->done_arg);
    free(io.out);

    pthread_mutex_lock(&lock);
    end_job_locked(j);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* Under lock. */
static uint16_t next_generation_locked(void) {
    if (++generations == 0) generations = 1;
    return generations;
}

/* Under lock, with nothing running: fills job (from a zeroed one) and
 * starts its worker. */
static bool spawn_job_locked(void * (*worker)(void *)) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return false;
    job.cancel_pipe[0] = pipefd[0];
    job.cancel_pipe[1] = pipefd[1];
    atomic_init(&job.cancelled, false);

    pthread_attr_t attr;
    pthread_t thread;
    bool started = pthread_attr_init(&attr) == 0;
    if (started) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&thread, &attr, worker, &job) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!started) {
        close(pipefd[0]);
        close(pipefd[1]);
        job.cancel_pipe[0] = job.cancel_pipe[1] = -1;
        return false;
    }
    job.running = true;
    return true;
}

/* Under lock, with nothing running. */
static uint16_t start_locked(yt_owner_t owner, const char * video_id, bool retry, yt_resolver_done_fn done,
                             void * arg) {
    memset(&job, 0, sizeof(job));
    job.generation = next_generation_locked();
    job.owner = owner;
    snprintf(job.video_id, sizeof(job.video_id), "%s", video_id);
    job.retry = retry;
    job.done = done;
    job.done_arg = arg;
    return spawn_job_locked(resolve_worker) ? job.generation : 0;
}

/* Under lock, with nothing running. On failure the caller keeps request. */
static uint16_t start_line_locked(uint16_t generation, yt_owner_t owner, yt_browse_kind_t kind, char * request,
                                  size_t request_len, yt_search_done_fn done, void * arg) {
    memset(&job, 0, sizeof(job));
    job.generation = generation;
    job.owner = owner;
    job.kind = kind;
    job.request = request;
    job.request_len = request_len;
    job.line_done = done;
    job.done_arg = arg;
    if (spawn_job_locked(line_worker)) return generation;
    job.request = NULL;
    return 0;
}

/* Under lock: the owner that requests are decided against. A search or a
 * page waiting for its prefetch or radio to be reaped already counts as
 * running. */
static yt_owner_t running_owner_locked(void) {
    if (pending.active) return pending.owner;
    return job.running ? job.owner : YT_OWNER_NONE;
}

static bool is_line_owner(yt_owner_t owner) {
    return owner == YT_OWNER_SEARCH || owner == YT_OWNER_BROWSE || owner == YT_OWNER_RADIO;
}

uint16_t yt_resolver_start(yt_owner_t owner, const char * video_id, yt_resolver_done_fn done, void * arg) {
    if (!yt_resolver_video_id_valid(video_id) || is_line_owner(owner)) return 0;
    pthread_mutex_lock(&lock);
    uint16_t generation = running_owner_locked() != YT_OWNER_NONE ? 0 : start_locked(owner, video_id, false, done, arg);
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
    bool cancelled;
    if (pending.active && pending.generation == generation) {
        cancelled = !pending.cancelled;
        pending.cancelled = true;
    } else {
        cancelled = cancel_locked(generation);
    }
    pthread_mutex_unlock(&lock);
    return cancelled;
}

/* Admits a line job (a search, a page or the radio) whose request line is
 * in line (len bytes, wiped here), under yt_job_admit(). */
static uint16_t start_line_job(yt_owner_t owner, yt_browse_kind_t kind, char * line, size_t len,
                               yt_search_done_fn done, void * arg, const char ** why) {
    char * request = malloc(len);
    if (request) memcpy(request, line, len);
    memset(line, 0, len);
    if (!request) {
        *why = "internal";
        return 0;
    }

    uint16_t generation = 0;
    pthread_mutex_lock(&lock);
    switch (yt_job_admit(running_owner_locked(), job.video_id, owner, NULL)) {
        case YT_REQ_START:
            generation = start_line_locked(next_generation_locked(), owner, kind, request, len, done, arg);
            if (!generation) *why = "internal";
            break;
        case YT_REQ_CANCEL_AND_START:
            /* The prefetch's or the radio's worker starts this one once
             * the running job is reaped (start_pending_locked()). */
            job.preempted = true;
            job.preempted_by = owner;
            cancel_locked(job.generation);
            generation = next_generation_locked();
            pending = (pending_line_t) { .active = true, .generation = generation, .owner = owner, .kind = kind,
                                         .request = request, .request_len = len, .done = done, .done_arg = arg };
            break;
        default:
            *why = "busy";
            break;
    }
    pthread_mutex_unlock(&lock);
    if (!generation) wipe_free(request, len);
    return generation;
}

uint16_t yt_resolver_start_search(const char * query, const char * token, yt_search_done_fn done, void * arg,
                                  const char ** why) {
    static const char * unused;
    if (!why) why = &unused;
    char line[YT_SEARCH_INPUT_MAX + 1];
    size_t len = yt_search_input_line(line, sizeof(line), query, token);
    if (len == 0) {
        *why = "bad_input";
        return 0;
    }
    return start_line_job(YT_OWNER_SEARCH, YT_BROWSE_KIND_COUNT, line, len, done, arg, why);
}

uint16_t yt_resolver_start_browse(yt_owner_t owner, yt_browse_kind_t kind, const char * id, const char * params,
                                  const char * token, yt_browse_done_fn done, void * arg, const char ** why) {
    static const char * unused;
    if (!why) why = &unused;
    char line[YT_BROWSE_INPUT_MAX + 1];
    size_t len = owner == YT_OWNER_BROWSE || (owner == YT_OWNER_RADIO && kind == YT_BROWSE_RELATED)
                     ? yt_browse_input_line(line, sizeof(line), kind, id, params, token)
                     : 0;
    if (len == 0) {
        *why = "bad_input";
        return 0;
    }
    return start_line_job(owner, kind, line, len, done, arg, why);
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
        switch (yt_job_admit(running_owner_locked(), job.video_id, owner, video_id)) {
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
                /* The radio gives way to the track that plays, and asks
                 * again later without counting a try. */
                if (job.owner == YT_OWNER_RADIO) {
                    job.preempted = true;
                    job.preempted_by = owner;
                }
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
    while ((job.running && job.generation == generation) || (pending.active && pending.generation == generation))
        pthread_cond_wait(&ended, &lock);
    pthread_mutex_unlock(&lock);
}

bool yt_resolver_busy(void) {
    pthread_mutex_lock(&lock);
    bool running = job.running || pending.active;
    pthread_mutex_unlock(&lock);
    return running;
}
