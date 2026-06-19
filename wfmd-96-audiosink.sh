#!/usr/bin/env bash
# Live WFM demo: Soapy IQ -> WFMD -> ALSA audiosink, while capturing both IQ and audio files.
#
# Defaults are for a local 96 MHz FM station. Override with env vars:
#   DEVICE=0 CF=96.0e6 SR=2400000 RF_BW=1.5e6 OUT_DIR=captures FORMAT=phcap DURATION=30 ./wfmd-96-audiosink.sh
#
# Outputs:
#   $OUT_DIR/live-<UTC>-iq.phcap      or .cf32 when FORMAT=raw
#   $OUT_DIR/live-<UTC>-audio.wav     by default; .phcap/.f32 via AUDIO_FORMAT

set -euo pipefail

SOCK=/tmp/.PhaseHound-broker.sock
PUB_TIMEOUT=${PUB_TIMEOUT:-2s}
PUB_GAP=${PUB_GAP:-0.03}
DEVICE=${DEVICE:-0}
CF=${CF:-96.0e6}
SR=${SR:-2400000}
RF_BW=${RF_BW:-1.5e6}
WFMD_BW=${WFMD_BW:-150000}
GAIN=${GAIN:-0.5}
OUT_DIR=${OUT_DIR:-captures}
FORMAT=${FORMAT:-phcap}        # phcap or raw (IQ capture format)
AUDIO_FORMAT=${AUDIO_FORMAT:-wav}  # wav, phcap or raw (audio capture format)
METADATA=${METADATA:-jsonl}    # used only for raw files
BLOCK=${BLOCK:-262144}
DURATION=${DURATION:-0}        # 0 = run until Ctrl-C
PLAY_AUDIO=${PLAY_AUDIO:-1}    # set 0 to capture only

case "$FORMAT" in raw|phcap) ;; *) echo "FORMAT must be raw or phcap" >&2; exit 2 ;; esac
case "$AUDIO_FORMAT" in raw|phcap|wav) ;; *) echo "AUDIO_FORMAT must be raw, phcap, or wav" >&2; exit 2 ;; esac

mkdir -p "$OUT_DIR"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
if [ "$FORMAT" = "raw" ]; then
  IQ_OUT="$OUT_DIR/live-${stamp}-iq.cf32"
else
  IQ_OUT="$OUT_DIR/live-${stamp}-iq.phcap"
fi
if [ "$AUDIO_FORMAT" = "wav" ]; then
  AUDIO_OUT="$OUT_DIR/live-${stamp}-audio.wav"
elif [ "$AUDIO_FORMAT" = "raw" ]; then
  AUDIO_OUT="$OUT_DIR/live-${stamp}-audio.f32"
else
  AUDIO_OUT="$OUT_DIR/live-${stamp}-audio.phcap"
fi

CORE_PID=""
SUB_PID=""
CLEANED=0
cleanup() {
  [ "$CLEANED" -eq 1 ] && return
  CLEANED=1
  set +e
  echo ""
  echo "[cleanup] filesink status before stop:"
  timeout 1s ./ph-cli pub filesink.config.in status 2>/dev/null || true
  echo ""
  pub filesink.config.in "stop" >/dev/null 2>&1 || true
  pub audiosink.config.in "stop" >/dev/null 2>&1 || true
  pub wfmd.config.in "stop" >/dev/null 2>&1 || true
  pub soapy.config.in "stop" >/dev/null 2>&1 || true
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

required_feeds=(soapy.config.in wfmd.config.in filesink.config.in)
if [ "$PLAY_AUDIO" != "0" ]; then
  required_feeds+=(audiosink.config.in)
fi
wait_for_feeds "${required_feeds[@]}"

./ph-cli sub \
  soapy.config.out wfmd.config.out wfmd.audio-info \
  filesink.config.out soapy.IQ-info audiosink.config.out &
SUB_PID=$!
sleep 0.1

# File capture: IQ in $FORMAT, audio independently in $AUDIO_FORMAT (default wav).
pub filesink.config.in "format $FORMAT"
pub filesink.config.in "audio-format $AUDIO_FORMAT"
pub filesink.config.in "metadata $METADATA"
pub filesink.config.in "block $BLOCK"
pub filesink.config.in "start_at live"
pub filesink.config.in "iq-path $IQ_OUT"
pub filesink.config.in "audio-path $AUDIO_OUT"
pub filesink.config.in "subscribe iq-source soapy.IQ-info"
pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
pub filesink.config.in "start"

# SDR source.
pub soapy.config.in "list"
pub soapy.config.in "select $DEVICE"
pub soapy.config.in "set sr=$SR cf=$CF bw=$RF_BW"

# Demodulator and optional live playback.
# audiosink must subscribe before "wfmd open" so it receives the audio ring memfd.
pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
if [ "$PLAY_AUDIO" != "0" ]; then
  pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
fi
pub wfmd.config.in "open"
pub wfmd.config.in "gain $GAIN"
pub wfmd.config.in "bw $WFMD_BW"

# Publish IQ memfd after subscribers are attached, then start the real-time path.
pub soapy.config.in "start"
sleep 0.1
pub soapy.config.in "open"
pub wfmd.config.in "start"
if [ "$PLAY_AUDIO" != "0" ]; then
  pub audiosink.config.in "start"
fi

printf '\nCapturing live WFM:\n  IQ:    %s\n  Audio: %s\n' "$IQ_OUT" "$AUDIO_OUT"
printf 'DURATION=%s, IQ_FORMAT=%s, AUDIO_FORMAT=%s. Press Ctrl-C to stop when DURATION=0.\n\n' "$DURATION" "$FORMAT" "$AUDIO_FORMAT"

if [ "$DURATION" != "0" ]; then
  sleep "$DURATION"
else
  wait "$SUB_PID"
fi
