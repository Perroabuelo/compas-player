#!/usr/bin/env bash
# hiby-yt: fragmented MP4 (fMP4/AAC) through mp4_demux against fixture files
# (the hiby-yt repo's tests/fixtures/fmp4). The demuxer part always runs,
# with sanitizers; decoding every sample also runs when the host build's
# FAAD2 objects exist (tools/build-base.sh --host in hiby-yt).
set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: test_mp4_frag.sh FIXTURE_DIR" >&2; exit 2; }
fixtures="$(cd "$1" && pwd)"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/mp4-frag"
mkdir -p "$build_dir"
sources=(src/audio/mp4_frag_spec.c src/audio/mp4_demux.c src/audio/mp4_frag.c src/audio/fmp4_index.c)

cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Isrc/audio "${sources[@]}" -o "$build_dir/demux"
"$build_dir/demux" "$fixtures"

shopt -s nullglob
faad=(build_host/faad2/*.o)
if [[ ${#faad[@]} -eq 0 ]]; then
    echo "mp4_frag_spec: no build_host/faad2, decoding skipped"
    exit 0
fi
cc -std=gnu11 -O1 -g -Wall -Wextra -DMP4_FRAG_SPEC_DECODE -Isrc/audio -Ifaad2/include \
    "${sources[@]}" src/audio/aac_decoder.c "${faad[@]}" -lm -o "$build_dir/decode"
"$build_dir/decode" "$fixtures"
