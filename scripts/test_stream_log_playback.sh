#!/usr/bin/env bash
# hiby-yt: plays network URLs through audio.c (host build, dummy audio) and
# prints the stream_log events they leave in ./.hiby-yt/stream.log of a
# fresh working directory. Needs network access to the URLs.
set -euo pipefail

[[ $# -ge 1 ]] || { echo "usage: test_stream_log_playback.sh URL..." >&2; exit 2; }
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build_test/stream_log_playback"
mkdir -p "$build_dir"

make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path '*/libsndfile/*' ! -name 'ogg_demux.o' ! -name 'main.o' -print)
gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/library" \
    $(sdl2-config --cflags) -c "$repo_root/src/audio/stream_log_playback_spec.c" \
    -o "$build_dir/stream_log_playback_spec.o"
g++ -pthread "$build_dir/stream_log_playback_spec.o" "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" "$repo_root/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lm -o "$build_dir/stream_log_playback_spec"

run_dir="$(mktemp -d)"
status=0
(cd "$run_dir" && SDL_AUDIODRIVER=dummy "$build_dir/stream_log_playback_spec" "$@") || status=$?
echo "--- .hiby-yt/stream.log"
cat "$run_dir/.hiby-yt/stream.log" 2>/dev/null || echo "(no log)"
rm -rf "$run_dir"
exit "$status"
