/* hiby-yt: fragmented MP4 through mp4_demux, against the fixtures of the
 * hiby-yt repo (tests/fixtures/fmp4: short.m4a, nosidx.m4a, truncated.m4a,
 * overflow.m4a and short.packets.tsv, ffprobe's packets of short.m4a).
 * Built by scripts/test_mp4_frag.sh; with -DMP4_FRAG_SPEC_DECODE it also
 * decodes every sample through aac_decoder (needs FAAD2). */
#include "mp4_demux.h"
#ifdef MP4_FRAG_SPEC_DECODE
#include "aac_decoder.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static const char * dir;
static unsigned ref_size[256];
static unsigned ref_count;

static const char * path(const char * name) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return buf;
}

static void load_reference(void) {
    FILE * f = fopen(path("short.packets.tsv"), "r");
    char line[256];
    if (!f || !fgets(line, sizeof(line), f)) {
        fprintf(stderr, "cannot read short.packets.tsv\n");
        exit(1);
    }
    while (ref_count < 256 && fgets(line, sizeof(line), f)) {
        unsigned long long pts, pos;
        char duration[16];
        if (sscanf(line, "%llu\t%15s\t%u\t%llu", &pts, duration, &ref_size[ref_count], &pos) != 4)
            exit(1);
        ref_count++;
    }
    fclose(f);
}

/* Reads samples in order from `first` until a read fails; returns how many. */
static unsigned read_in_order(mp4_demux_t * d, unsigned first, bool check_sizes) {
    uint8_t buf[8192];
    uint32_t size;
    unsigned n = first;
    while (mp4_demux_read_sample(d, n, buf, sizeof(buf), &size)) {
        if (check_sizes)
            CHECK(n < ref_count && size == ref_size[n]);
        n++;
    }
    return n - first;
}

static void test_short(void) {
    mp4_demux_t * d = mp4_demux_open(path("short.m4a"));
    CHECK(d != NULL);
    if (!d)
        return;
    mp4_frag_t * m = mp4_demux_get_frag(d);
    CHECK(m && mp4_frag_has_sidx(m) && mp4_frag_sidx_refs(m) == 5);
    CHECK(mp4_frag_can_seek(m) && mp4_frag_length_known(m) && !mp4_frag_var_duration(m));
    CHECK(mp4_demux_get_sample_count(d) == ref_count && ref_count == 215);
    CHECK(mp4_frag_sample_ticks(m) == 1024 && mp4_demux_get_frames_per_sample(d) == 1024);
    CHECK(mp4_demux_get_total_pcm_frame_count(d) == 215 * 1024);
    uint32_t frag;
    uint64_t offset;
    CHECK(mp4_frag_locate(m, 100, &frag, &offset) && frag == 2 && offset == 17482);
    CHECK(mp4_frag_locate(m, 86, &frag, &offset) && frag == 2);
    CHECK(mp4_frag_locate(m, 85, &frag, &offset) && frag == 1);
    CHECK(!mp4_frag_locate(m, 215, &frag, &offset));

    CHECK(read_in_order(d, 0, true) == 215);
    CHECK(mp4_frag_error_stage(m) == NULL);
    /* Jumps load the fragment from sidx: backwards, forwards, the last one. */
    uint8_t buf[8192];
    uint32_t size;
    const unsigned jumps[] = {100, 3, 214, 43, 42, 170};
    for (unsigned i = 0; i < sizeof(jumps) / sizeof(jumps[0]); i++)
        CHECK(mp4_demux_read_sample(d, jumps[i], buf, sizeof(buf), &size) && size == ref_size[jumps[i]]);
    uint32_t sample;
    uint64_t discard;
    CHECK(mp4_demux_map_pcm_frame(d, 100 * 1024 + 7, &sample, &discard) && sample == 100 && discard == 7);
    CHECK(!mp4_demux_read_sample(d, 215, buf, sizeof(buf), &size));
    mp4_demux_close(d);
}

static void test_nosidx(void) {
    mp4_demux_t * d = mp4_demux_open(path("nosidx.m4a"));
    CHECK(d != NULL);
    if (!d)
        return;
    mp4_frag_t * m = mp4_demux_get_frag(d);
    /* ffmpeg declares no duration and there is no sidx: unknown length. */
    CHECK(m && !mp4_frag_has_sidx(m) && !mp4_frag_can_seek(m) && !mp4_frag_length_known(m));
    uint8_t buf[8192];
    uint32_t size;
    uint32_t frag;
    uint64_t offset;
    CHECK(!mp4_frag_locate(m, 100, &frag, &offset));
    CHECK(!mp4_demux_read_sample(d, 100, buf, sizeof(buf), &size)); /* no jumps */
    CHECK(read_in_order(d, 0, true) == 215);
    CHECK(mp4_frag_length_known(m) && mp4_demux_get_sample_count(d) == 215);
    CHECK(mp4_frag_error_stage(m) == NULL);
    mp4_demux_close(d);
}

static void test_corrupt(const char * name, unsigned good, const char * stage) {
    mp4_demux_t * d = mp4_demux_open(path(name));
    CHECK(d != NULL);
    if (!d)
        return;
    mp4_frag_t * m = mp4_demux_get_frag(d);
    CHECK(read_in_order(d, 0, true) == good);
    CHECK(m && mp4_frag_error_stage(m) && !strcmp(mp4_frag_error_stage(m), stage));
    /* A failed reader stays failed instead of skipping ahead. */
    uint8_t buf[8192];
    uint32_t size;
    CHECK(!mp4_demux_read_sample(d, good + 50, buf, sizeof(buf), &size));
    mp4_demux_close(d);
}

#ifdef MP4_FRAG_SPEC_DECODE
static uint64_t decode_all(const char * name, decoder_read_status_t * last) {
    aac_decoder_t * a = aac_open_file_mp4(path(name));
    CHECK(a != NULL);
    if (!a)
        return 0;
    static int16_t pcm[1024 * 8];
    uint64_t n = 0;
    *last = DECODER_READ_OK;
    for (;;) {
        decoder_read_result_t r = aac_read_pcm_frames_s16(a, 1024, pcm);
        n += r.frames;
        if (!r.frames && r.status != DECODER_READ_RECOVERABLE_ERROR) {
            *last = r.status;
            break;
        }
    }
    aac_close(a);
    return n;
}

static void test_decode(void) {
    decoder_read_status_t last;
    aac_decoder_t * a = aac_open_file_mp4(path("short.m4a"));
    CHECK(a && aac_get_channels(a) == 1 && aac_get_sample_rate(a) == 22050);
    uint64_t total = a ? aac_get_total_pcm_frame_count(a) : 0;
    CHECK(total > 200 * 1024 && total <= 215 * 1024);
    /* Seeking inside the fragmented track works with sidx. */
    CHECK(a && aac_seek_to_pcm_frame(a, 3 * 44032 + 100));
    aac_close(a);

    uint64_t frames = decode_all("short.m4a", &last);
    CHECK(frames == total && last == DECODER_READ_EOF);
    /* Without sidx and duration, it decodes the same audio to a clean EOF. */
    frames = decode_all("nosidx.m4a", &last);
    CHECK(frames == total && last == DECODER_READ_EOF);
    a = aac_open_file_mp4(path("nosidx.m4a"));
    CHECK(a && !aac_seek_to_pcm_frame(a, 3 * 44032 + 100));
    aac_close(a);
    /* A truncated track ends in an error, not a clean end. */
    frames = decode_all("truncated.m4a", &last);
    CHECK(frames < total && last == DECODER_READ_FATAL_ERROR);
    printf("mp4_frag_spec: decoded %llu frames\n", (unsigned long long)total);
}
#endif

int main(int argc, char ** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mp4_frag_spec FIXTURE_DIR\n");
        return 2;
    }
    dir = argv[1];
    load_reference();
    test_short();
    test_nosidx();
    test_corrupt("truncated.m4a", 86, "moof");
    test_corrupt("overflow.m4a", 43, "limit");
#ifdef MP4_FRAG_SPEC_DECODE
    test_decode();
#endif
    if (failures) {
        fprintf(stderr, "mp4_frag_spec: %d failure(s)\n", failures);
        return 1;
    }
    printf("mp4_frag_spec: ok\n");
    return 0;
}
