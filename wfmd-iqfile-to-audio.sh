#!/usr/bin/env bash
# Replay an IQ file through WFMD and capture the demodulated audio file.
#
# Usage:
#   ./wfmd-iqfile-to-audio.sh <input-iq-or-phcap> [output-audio]
#
# For raw IQ input, set metadata explicitly:
#   TYPE=iq-cf32 SR=2400000 CF=96.0e6 ./wfmd-iqfile-to-audio.sh in.cf32 out.phcap
#
# For PHCAP input, type/rate/frequency are read from the file header.

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <input-iq-or-phcap> [output-audio]" >&2
  exit 2
fi

SOCK=/tmp/.PhaseHound-broker.sock
PUB_TIMEOUT=${PUB_TIMEOUT:-2s}
PUB_GAP=${PUB_GAP:-0.03}
IQ_IN=$1

TYPE=${TYPE:-iq-cf32}
SR=${SR:-2400000}
CF=${CF:-0}
IN_FORMAT=${IN_FORMAT:-auto}      # auto, raw, phcap
OUT_FORMAT_WAS_SET=${OUT_FORMAT+x}
OUT_FORMAT=${OUT_FORMAT:-wav}     # wav, phcap or raw
OUT_METADATA=${OUT_METADATA:-jsonl}
BLOCK=${BLOCK:-262144}
RING=${RING:-8388608}
THROTTLE=${THROTTLE:-1}           # 1 is safer: no backpressure yet for offline burst replay
LOOP=${LOOP:-0}
GAIN=${GAIN:-0.5}
WFMD_BW=${WFMD_BW:-150000}
DURATION=${DURATION:-auto}        # auto = estimate from file, 0 = run until Ctrl-C
DRAIN_SEC=${DRAIN_SEC:-1}         # extra drain window after estimated replay duration
PLAY_AUDIO=${PLAY_AUDIO:-1}       # set 0 to convert only, no audiosink playback

# An explicit output filename selects its obvious format unless OUT_FORMAT was
# explicitly supplied. This avoids writing WAV data into a .phcap/.f32 path.
if [ -n "${2:-}" ] && [ -z "${OUT_FORMAT_WAS_SET:-}" ]; then
  case "${2,,}" in
    *.wav) OUT_FORMAT=wav ;;
    *.phcap) OUT_FORMAT=phcap ;;
    *.f32|*.raw) OUT_FORMAT=raw ;;
  esac
fi

case "$IN_FORMAT" in auto|raw|phcap) ;; *) echo "IN_FORMAT must be auto, raw, or phcap" >&2; exit 2 ;; esac
case "$OUT_FORMAT" in raw|phcap|wav) ;; *) echo "OUT_FORMAT must be raw, phcap, or wav" >&2; exit 2 ;; esac

# Default output extension matches format.
if [ -n "${2:-}" ]; then
  OUT=$2
elif [ "$OUT_FORMAT" = "wav" ]; then
  OUT="${IQ_IN%.*}-wfmd-audio.wav"
elif [ "$OUT_FORMAT" = "raw" ]; then
  OUT="${IQ_IN%.*}-wfmd-audio.f32"
else
  OUT="${IQ_IN%.*}-wfmd-audio.phcap"
fi

if [ ! -r "$IQ_IN" ]; then
  echo "input not readable: $IQ_IN" >&2
  exit 1
fi

if [ "$IN_FORMAT" = "auto" ]; then
  if [ "$(LC_ALL=C dd if="$IQ_IN" bs=7 count=1 2>/dev/null | tr -d '\000')" = "PHCAP00" ]; then
    IN_FORMAT=phcap
  else
    IN_FORMAT=raw
  fi
fi

estimate_replay_seconds() {
  python3 - "$IQ_IN" "$IN_FORMAT" "$TYPE" "$SR" <<'PYEOF'
import math, os, struct, sys
path, fmt, typ, sr_s = sys.argv[1:5]
try:
    sr = float(sr_s)
except Exception:
    sr = 0.0

def raw_frame_bytes(t):
    t = t.lower()
    if t in ('iq-cf32','cf32'): return 8
    if t in ('iq-cs16','cs16'): return 4
    if t in ('audio-f32','pcm-f32','f32'): return 4
    if t in ('audio-s16','pcm-s16','s16'): return 2
    return 0
try:
    if fmt == 'raw':
        fb = raw_frame_bytes(typ)
        if fb <= 0 or sr <= 0: raise ValueError('bad raw metadata')
        print(max(1, int(math.ceil(os.path.getsize(path) / fb / sr))))
    elif fmt == 'phcap':
        with open(path,'rb') as f:
            h = f.read(128)
            if len(h) != 128 or h[:7] != b'PHCAP00': raise ValueError('bad phcap')
            vals = struct.unpack('<8sIIIIIIddIIII64s', h)
            # magic, version, header_bytes, kind, encoding, channels, bytes_per_samp, sample_rate...
            kind = vals[3]
            channels = vals[5] or 1
            bps = vals[6] or 1
            sr = vals[7]
            frame = bps if kind == 1 else bps * channels
            total = 0
            while True:
                bh = f.read(84)
                if not bh: break
                if len(bh) != 84 or bh[:8] != b'PHBLK00\0': break
                payload = struct.unpack_from('<Q', bh, 24)[0]
                total += payload
                f.seek(payload, os.SEEK_CUR)
            if frame <= 0 or sr <= 0: raise ValueError('bad phcap metadata')
            print(max(1, int(math.ceil(total / frame / sr))))
    else:
        raise ValueError('unknown format')
except Exception:
    print(0)
PYEOF
}

if [ "$DURATION" = "auto" ]; then
  est=$(estimate_replay_seconds)
  if [ "${est:-0}" != "0" ]; then
    DURATION=$((est + DRAIN_SEC + 1))
  else
    DURATION=0
  fi
fi

CORE_PID=""
SUB_PID=""
WF_PID=""
CLEANED=0
cleanup() {
  [ "$CLEANED" -eq 1 ] && return
  CLEANED=1
  set +e
  pub filesink.config.in "stop" >/dev/null 2>&1 || true
  pub audiosink.config.in "stop" >/dev/null 2>&1 || true
  pub wfmd.config.in "stop" >/dev/null 2>&1 || true
  pub filesource.config.in "stop" >/dev/null 2>&1 || true
  if [ -n "${WF_PID:-}" ]; then
    kill "$WF_PID" >/dev/null 2>&1 || true
    wait "$WF_PID" >/dev/null 2>&1 || true
  fi
  if [ -n "${SUB_PID:-}" ]; then
    kill "$SUB_PID" >/dev/null 2>&1 || true
    wait "$SUB_PID" >/dev/null 2>&1 || true
  fi
  if [ -n "${CORE_PID:-}" ]; then
    kill -INT "$CORE_PID" >/dev/null 2>&1 || true
    wait "$CORE_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

pub() {
  local feed=$1 command=$2
  if ! timeout "$PUB_TIMEOUT" ./ph-cli pub "$feed" "$command" >/dev/null; then
    echo "command dispatch failed: $feed <- $command" >&2
    return 1
  fi
  sleep "$PUB_GAP"
}

wait_for_feeds() {
  local deadline=$((SECONDS + 8)) feeds missing feed
  while (( SECONDS < deadline )); do
    feeds=$(./ph-cli list feeds 2>/dev/null || true)
    missing=0
    for feed in "$@"; do
      if ! grep -Fq '"feed":"'"$feed"'"' <<<"$feeds"; then
        missing=1
        break
      fi
    done
    (( missing == 0 )) && return 0
    sleep 0.15
  done
  echo "required addon feeds did not become ready: $*" >&2
  return 1
}

if [ ! -S "$SOCK" ]; then
  ./ph-core &
  CORE_PID=$!
  sleep 0.5
fi

required_feeds=(filesource.config.in wfmd.config.in filesink.config.in)
if [ "$PLAY_AUDIO" != "0" ]; then
  required_feeds+=(audiosink.config.in)
fi
wait_for_feeds "${required_feeds[@]}"

./ph-cli sub filesource.config.out filesource.IQ-info wfmd.config.out wfmd.audio-info filesink.config.out audiosink.config.out &
SUB_PID=$!
sleep 0.1

# Audio capture target.
pub filesink.config.in "format $OUT_FORMAT"
pub filesink.config.in "metadata $OUT_METADATA"
pub filesink.config.in "block $BLOCK"
pub filesink.config.in "start_at live"
pub filesink.config.in "audio-path $OUT"
pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
pub filesink.config.in "start"

# Subscribe audiosink BEFORE wfmd open so it receives the audio ring memfd.
if [ "$PLAY_AUDIO" != "0" ]; then
  pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
fi

# Demodulator — open publishes the audio ring to current subscribers.
pub wfmd.config.in "subscribe iq-source filesource.IQ-info"
pub wfmd.config.in "open"
pub wfmd.config.in "gain $GAIN"
pub wfmd.config.in "bw $WFMD_BW"
pub wfmd.config.in "start"

if [ "$PLAY_AUDIO" != "0" ]; then
  pub audiosink.config.in "start"
fi

# File source. For raw, the user must provide TYPE/SR/CF correctly.
pub filesource.config.in "path $IQ_IN"
pub filesource.config.in "format $IN_FORMAT"
pub filesource.config.in "block $BLOCK"
pub filesource.config.in "ring $RING"
pub filesource.config.in "loop $LOOP"
pub filesource.config.in "throttle $THROTTLE"
if [ "$IN_FORMAT" = "raw" ]; then
  pub filesource.config.in "type $TYPE"
  pub filesource.config.in "sr $SR"
  pub filesource.config.in "cf $CF"
  pub filesource.config.in "metadata latest"
fi
# Start waterfall BEFORE filesource publishes the ring so it receives the memfd.
if [ -x ./ph-waterfall ]; then
  ./ph-waterfall --feed filesource.IQ-info &
  WF_PID=$!
  echo "[waterfall] launched (PID=$WF_PID) — close window or press Q to stop"
  sleep 0.15   # give recv_thread time to connect and subscribe
fi

pub filesource.config.in "start"

printf '\nWFMD replay:\n  IQ in:  %s (%s)\n  Audio:  %s (%s)\n' "$IQ_IN" "$IN_FORMAT" "$OUT" "$OUT_FORMAT"
printf 'PLAY_AUDIO=%s. THROTTLE=%s. DURATION=%s seconds (0 means manual Ctrl-C).\n\n' "$PLAY_AUDIO" "$THROTTLE" "$DURATION"

if [ "$DURATION" != "0" ]; then
  sleep "$DURATION"
else
  wait "$SUB_PID"
fi
