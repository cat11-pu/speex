#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${MODE:-nb}
FS=${FS:-8000}
FRAME_SIZE=${FRAME_SIZE:-160}
LOSS_PERCENT=${LOSS_PERCENT:-20}
SEED=${SEED:-12345}
BASE_REF=${BASE_REF:-HEAD}

case "$MODE" in
  nb) FS=8000; FRAME_SIZE=160 ;;
  wb) FS=16000; FRAME_SIZE=320 ;;
  *) echo "MODE must be nb or wb" >&2; exit 2 ;;
esac

TMP=$(mktemp -d)
trap 'git -C "$ROOT" worktree remove --force "$TMP/base-src" >/dev/null 2>&1 || true; rm -rf "$TMP"' EXIT

if [ "$#" -ge 1 ]; then
  INPUT=$1
else
  INPUT=$TMP/speech.wav
  ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "flite=text='The quick brown fox jumps over the lazy dog. Speech quality is measured across a noisy packet network. Please confirm the meeting time and read back the numbers clearly.':voice=kal" \
    -ar "$FS" -ac 1 -sample_fmt s16 -y "$INPUT"
fi

ffmpeg -hide_banner -loglevel error -i "$INPUT" -f s16le -acodec pcm_s16le -y "$TMP/reference.raw"

git -C "$ROOT" worktree add --detach "$TMP/base-src" "$BASE_REF" >/dev/null

meson setup "$TMP/build-base" "$TMP/base-src" \
  -Dfixed-point=enabled -Dtools=disabled -Dtest-binaries=disabled >/dev/null
meson compile -C "$TMP/build-base" >/dev/null

meson setup "$TMP/build-new" "$ROOT" \
  -Dfixed-point=enabled -Dtools=disabled -Dtest-binaries=disabled >/dev/null
meson compile -C "$TMP/build-new" >/dev/null

build_harness() {
  local src=$1
  local build=$2
  local out=$3
  cc -O2 -I"$src/include" -I"$src/include/speex" \
    -I"$build" -I"$build/include/speex" \
    "$ROOT/tests/plc_eval.c" \
    -L"$build/libspeex" -Wl,-rpath,"$build/libspeex" \
    -lspeex -lm -o "$out"
}

build_harness "$TMP/base-src" "$TMP/build-base" "$TMP/plc_eval_base"
build_harness "$ROOT" "$TMP/build-new" "$TMP/plc_eval_new"

"$TMP/plc_eval_base" -m "$MODE" -q 8 -l "$LOSS_PERCENT" -s "$SEED" \
  "$TMP/reference.raw" "$TMP/clean-base.raw" "$TMP/lossy-base.raw" 2>"$TMP/base.log"
"$TMP/plc_eval_new" -m "$MODE" -q 8 -l "$LOSS_PERCENT" -s "$SEED" \
  "$TMP/reference.raw" "$TMP/clean-new.raw" "$TMP/lossy-new.raw" 2>"$TMP/new.log"

cat "$TMP/base.log"
cat "$TMP/new.log"

echo
echo "== baseline =="
python3 "$ROOT/tests/plc_metrics.py" "$TMP/reference.raw" \
  "$TMP/clean-base.raw" "$TMP/lossy-base.raw" "$TMP/lossy-base.raw.loss" \
  --fs "$FS" --frame-size "$FRAME_SIZE"

echo
echo "== patched =="
python3 "$ROOT/tests/plc_metrics.py" "$TMP/reference.raw" \
  "$TMP/clean-new.raw" "$TMP/lossy-new.raw" "$TMP/lossy-new.raw.loss" \
  --fs "$FS" --frame-size "$FRAME_SIZE"

if [ -n "${PESQ_CLI:-}" ]; then
  echo
  echo "== PESQ =="
  if [ "$MODE" = wb ]; then
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/clean-new.raw" "$FS" wb | tail -1
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/lossy-base.raw" "$FS" wb | tail -1
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/lossy-new.raw" "$FS" wb | tail -1
  else
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/clean-new.raw" "$FS" | tail -1
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/lossy-base.raw" "$FS" | tail -1
    "$PESQ_CLI" "$TMP/reference.raw" "$TMP/lossy-new.raw" "$FS" | tail -1
  fi
fi
