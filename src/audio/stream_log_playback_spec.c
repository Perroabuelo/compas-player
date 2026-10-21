/* hiby-yt: plays network tracks through audio.c to their end and leaves
 * the stream_log events in ./.hiby-yt/stream.log (host build). A seekable
 * track is sought to 120 s and then to 5 s before its end, so a 4-minute
 * track takes seconds. Built by scripts/test_stream_log_playback.sh. */
#include "audio.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Defined in main.c, which this harness does not link. */
void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void);
void main_loop_discard_elapsed_time(void) {}

static void sleep_ms(long ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static bool wait_format(audio_current_format_info_t * out, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        if (audio_get_current_format_info(out))
            return true;
        sleep_ms(10);
    }
    return false;
}

static bool wait_finished(int seconds) {
    for (int i = 0; i < seconds * 10; i++) {
        if (audio_consume_track_finished() || audio_is_idle())
            return true;
        sleep_ms(100);
    }
    return false;
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: stream_log_playback_spec URL...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN); /* as main.c does */
    audio_init();
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        audio_current_format_info_t info;
        audio_consume_track_finished();
        audio_play_file_at(argv[i], 0.0, false, 0.0, false, 0.0);
        if (!wait_format(&info, 30)) {
            fprintf(stderr, "%s: no format (open failed)\n", argv[i]);
            wait_finished(5);
            continue;
        }
        fprintf(stderr, "%s: seekable=%d duration=%.2f s\n", argv[i], info.seekable, info.duration_seconds);
        sleep_ms(2000);
        if (info.seekable && info.duration_seconds > 130) {
            audio_seek(120.0);
            sleep_ms(2000);
            fprintf(stderr, "  after seek(120): position=%.2f s\n", audio_get_position_seconds());
            audio_seek(info.duration_seconds - 5.0);
        }
        bool done = wait_finished(info.seekable ? 60 : 300);
        fprintf(stderr, "  finished=%d\n", done);
        if (!done) {
            failures++;
            audio_stop();
        }
        sleep_ms(500);
    }
    audio_stop();
    sleep_ms(500);
    return failures ? 1 : 0;
}
