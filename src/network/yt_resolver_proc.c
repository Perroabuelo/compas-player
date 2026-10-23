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
    yt_resolver_done_fn done;
    void * done_arg;

    /* Worker only, until done returns. */
    yt_resolver_result_t result;
} resolve_job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended = PTHREAD_COND_INITIALIZER;
static resolve_job_t job = { .cancel_pipe = { -1, -1 } };
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
    if (!id) return false;
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= YT_RESOLVER_VIDEO_ID_LEN) return false;
    }
    return n == YT_RESOLVER_VIDEO_ID_LEN;
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

static void * resolve_worker(void * arg) {
    resolve_job_t * j = arg;
    uint64_t start = now_ms();
    char exit_buf[24];
    const char * exit_desc = run_resolver(j, start + (uint64_t) timeout_ms, exit_buf, sizeof(exit_buf));
    j->result.cancelled = atomic_load(&j->cancelled);
    set_log_class(&j->result);
    stream_log_event(stream_log_next_sid(), "resolve", "video_id=%s class=%s ms=%llu exit=%s", j->video_id,
                     j->result.cls, (unsigned long long) (now_ms() - start), exit_desc);
    if (j->done) j->done(&j->result, j->done_arg);

    pthread_mutex_lock(&lock);
    close(j->cancel_pipe[0]);
    close(j->cancel_pipe[1]);
    j->cancel_pipe[0] = j->cancel_pipe[1] = -1;
    j->running = false;
    pthread_cond_broadcast(&ended);
    pthread_mutex_unlock(&lock);
    return NULL;
}

uint16_t yt_resolver_start(yt_owner_t owner, const char * video_id, yt_resolver_done_fn done, void * arg) {
    if (!yt_resolver_video_id_valid(video_id)) return 0;
    pthread_mutex_lock(&lock);
    if (job.running) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    uint16_t generation = (uint16_t) (job.generation + 1);
    if (generation == 0) generation = 1;
    memset(&job, 0, sizeof(job));
    job.generation = generation;
    job.owner = owner;
    job.cancel_pipe[0] = pipefd[0];
    job.cancel_pipe[1] = pipefd[1];
    atomic_init(&job.cancelled, false);
    snprintf(job.video_id, sizeof(job.video_id), "%s", video_id);
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
        pthread_mutex_unlock(&lock);
        return 0;
    }
    job.running = true;
    pthread_mutex_unlock(&lock);
    return generation;
}

bool yt_resolver_cancel(uint16_t generation) {
    pthread_mutex_lock(&lock);
    bool cancelled = false;
    if (job.running && job.generation == generation && !atomic_exchange(&job.cancelled, true)) {
        char b = 1;
        ssize_t w = write(job.cancel_pipe[1], &b, 1);
        (void) w; /* the worker also checks the flag; a full pipe still wakes it */
        cancelled = true;
    }
    pthread_mutex_unlock(&lock);
    return cancelled;
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
