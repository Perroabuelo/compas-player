#include "remote_track.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Guards remote_track_table/remote_track_table_count -- written from the UI
 * thread only (gui_plugin_play_remote_tracks() -> remote_track_meta_set_all()),
 * read from the UI thread AND the audio/seek-worker threads (decoder_open()
 * -> remote_track_meta_copy_for_path()). See remote_track_meta_copy_for_path()'s
 * own header comment for the exact race this closes. */
static pthread_mutex_t remote_track_mutex = PTHREAD_MUTEX_INITIALIZER;
static remote_track_meta_t * remote_track_table = NULL;
static int remote_track_table_count = 0;

bool remote_track_make_key(const char * provider, const char * track_id, char * out, size_t out_size) {
    if (!provider || !provider[0] || !track_id || !track_id[0]) return false;
    for (const char * p = provider; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    for (const char * p = track_id; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    int n = snprintf(out, out_size, "remote://%s/%s", provider, track_id);
    return n > 0 && (size_t) n < out_size;
}

bool remote_track_path_is_remote(const char * path) {
    return path && strncmp(path, "remote://", 9) == 0;
}

bool remote_track_meta_set_all(const remote_track_meta_t * entries, int count) {
    if (count < 0) count = 0;
    if (count > REMOTE_TRACK_MAX_ENTRIES) count = REMOTE_TRACK_MAX_ENTRIES;

    remote_track_meta_t * new_table = NULL;
    if (count > 0) {
        /* Validate every entry's key BEFORE touching any shared state or
         * allocating -- an all-or-nothing publish, never a partially
         * replaced table (a plugin's own malformed entry shouldn't be able
         * to wipe out a previously-working remote queue). */
        char key[256];
        for (int i = 0; i < count; i++) {
            if (!remote_track_make_key(entries[i].provider, entries[i].track_id, key, sizeof(key))) return false;
        }
        new_table = malloc(sizeof(remote_track_meta_t) * (size_t) count);
        if (!new_table) return false;
        memcpy(new_table, entries, sizeof(remote_track_meta_t) * (size_t) count);
    }

    pthread_mutex_lock(&remote_track_mutex);
    remote_track_meta_t * old_table = remote_track_table;
    remote_track_table = new_table;
    remote_track_table_count = new_table ? count : 0;
    pthread_mutex_unlock(&remote_track_mutex);

    /* Freed AFTER the swap, outside the lock -- any reader that was mid-
     * copy under the lock already finished (and released it) before this
     * unlock could have happened; remote_track_meta_copy_for_path() never
     * hands out a pointer that could still be in use here. */
    free(old_table);

    return true;
}

static bool key_kept(const char * key, const char * const * keep_keys, int keep_count) {
    for (int i = 0; i < keep_count; i++) {
        if (keep_keys[i] && strcmp(keep_keys[i], key) == 0) return true;
    }
    return false;
}

remote_track_append_t remote_track_meta_append(const remote_track_meta_t * entry, const char * const * keep_keys,
                                               int keep_count) {
    char new_key[256], key[256];
    if (!entry || !remote_track_make_key(entry->provider, entry->track_id, new_key, sizeof(new_key)))
        return REMOTE_TRACK_APPEND_INVALID;
    if (keep_count < 0 || !keep_keys) keep_count = 0;

    /* The UI thread is the table's only writer, so it reads the current
     * table without the lock; only the swap below needs it. */
    const remote_track_meta_t * old = remote_track_table;
    int old_count = remote_track_table_count;
    int kept = 0;
    for (int i = 0; i < old_count; i++) {
        if (!remote_track_make_key(old[i].provider, old[i].track_id, key, sizeof(key))) continue;
        if (strcmp(key, new_key) != 0 && key_kept(key, keep_keys, keep_count)) kept++;
    }
    if (kept + 1 > REMOTE_TRACK_MAX_ENTRIES) return REMOTE_TRACK_APPEND_FULL;

    remote_track_meta_t * new_table = malloc(sizeof(remote_track_meta_t) * (size_t) (kept + 1));
    if (!new_table) return REMOTE_TRACK_APPEND_NO_MEMORY;
    int n = 0;
    for (int i = 0; i < old_count; i++) {
        if (!remote_track_make_key(old[i].provider, old[i].track_id, key, sizeof(key))) continue;
        if (strcmp(key, new_key) != 0 && key_kept(key, keep_keys, keep_count)) new_table[n++] = old[i];
    }
    new_table[n++] = *entry;

    pthread_mutex_lock(&remote_track_mutex);
    remote_track_meta_t * old_table = remote_track_table;
    remote_track_table = new_table;
    remote_track_table_count = n;
    pthread_mutex_unlock(&remote_track_mutex);
    /* After the swap, outside the lock: see remote_track_meta_set_all(). */
    free(old_table);
    return REMOTE_TRACK_APPEND_OK;
}

bool remote_track_meta_copy_for_path(const char * path, remote_track_meta_t * out) {
    if (!remote_track_path_is_remote(path)) return false;
    const char * rest = path + 9; /* skip the "remote://" prefix already confirmed present */
    const char * slash = strchr(rest, '/');
    if (!slash) return false; /* malformed -- no provider/track_id separator */
    size_t provider_len = (size_t) (slash - rest);
    const char * track_id = slash + 1;
    bool found = false;
    pthread_mutex_lock(&remote_track_mutex);
    for (int i = 0; i < remote_track_table_count; i++) {
        if (strlen(remote_track_table[i].provider) == provider_len &&
            strncmp(remote_track_table[i].provider, rest, provider_len) == 0 &&
            strcmp(remote_track_table[i].track_id, track_id) == 0) {
            *out = remote_track_table[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&remote_track_mutex);
    return found;
}
