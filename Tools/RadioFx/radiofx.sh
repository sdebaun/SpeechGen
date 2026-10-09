#!/usr/bin/env bash
# radiofx.sh: turn one clean TTS line into one handheld radio transmission (a working handheld, not a destroyed one).
#
# Usage: radiofx.sh [-c] in.wav out.wav
#   -c   clean mode: trim and level-match only, no radio (the "A" side of an A/B pair)
#
# Chain, in order:
#   1. trim leading/trailing silence (Kokoro pads ~0.3-0.4 s before and ~0.6 s after speech), keep 25 ms of air
#   2. mic band-pass 300-3400 Hz (steep FIR)
#   3. compand: fast AGC that lifts quiet syllables and squashes peaks, like a handheld's mic limiter
#   4. light overdrive: preamp/speaker grit
#   5. speaker band-pass 300-3400 Hz again, so the grit stays in band
#   6. low band-limited hiss under the whole transmission, starting with the key-up
#   7. key-up beep (talk-permit tone) before the voice
#   8. squelch tail after the voice: a short noise burst that cuts off fast
#
# Requires sox (brew install sox). Deterministic: sox -R seeds the noise generators.
# Tuned to the same middle grit as SpeechGenRadio's Mid preset, over a whole montage: crest about 6.1 inside
# transmissions and the hiss about 29 dB under the voice (the first version, drive 7 / hiss -32, measured 6.8 and 32: too clear).
# Tunables via env: RADIOFX_RATE (24000) RADIOFX_HISS_DB (-28) RADIOFX_DRIVE (7.8) RADIOFX_BEEP_HZ (1250)
#                   RADIOFX_TAIL_S (0.16) RADIOFX_TAIL_DB (-9)
set -euo pipefail

clean=0
if [[ "${1:-}" == "-c" ]]; then clean=1; shift; fi
if [[ $# -ne 2 ]]; then
	echo "usage: $0 [-c] in.wav out.wav" >&2
	exit 2
fi
in=$1
out=$2

rate=${RADIOFX_RATE:-24000}
hiss_db=${RADIOFX_HISS_DB:--28}
drive=${RADIOFX_DRIVE:-7.8}
beep_hz=${RADIOFX_BEEP_HZ:-1250}
tail_s=${RADIOFX_TAIL_S:-0.16}
tail_db=${RADIOFX_TAIL_DB:--9}

sox=(sox -R -V1)
fmt=(-r "$rate" -c 1 -b 16)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 1. trim: silence effect from the front, then reversed for the back
"${sox[@]}" "$in" "${fmt[@]}" "$tmp/trim.wav" \
	silence 1 0.015 -48d reverse silence 1 0.015 -48d reverse pad 0.025 0.025

if [[ $clean -eq 1 ]]; then
	"${sox[@]}" "$tmp/trim.wav" "$out" gain -n -3
	exit 0
fi

# 2-5. the voice path
"${sox[@]}" "$tmp/trim.wav" "$tmp/voice.wav" \
	sinc 300-3400 \
	compand 0.003,0.10 6:-70,-70,-50,-32,-30,-16,-10,-9,0,-6 -2 -70 0.003 \
	overdrive "$drive" 15 \
	sinc 300-3400 \
	gain -n -4

# 7. key-up: 40 ms of open carrier, the beep, 50 ms of carrier before the talker starts
"${sox[@]}" -n "${fmt[@]}" "$tmp/pre.wav" trim 0 0.04
"${sox[@]}" -n "${fmt[@]}" "$tmp/beep.wav" synth 0.07 sine "$beep_hz" fade h 0.004 0.07 0.012 gain -21
"${sox[@]}" -n "${fmt[@]}" "$tmp/gap.wav" trim 0 0.05
"${sox[@]}" "$tmp/pre.wav" "$tmp/beep.wav" "$tmp/gap.wav" "$tmp/voice.wav" "$tmp/dry.wav"

# 6. hiss the length of the transmission, band-limited like the voice
dur=$(soxi -D "$tmp/dry.wav")
"${sox[@]}" -n "${fmt[@]}" "$tmp/hiss.wav" synth "$dur" whitenoise sinc 300-3400 gain "$hiss_db"
"${sox[@]}" -m -v 1 "$tmp/dry.wav" -v 1 "$tmp/hiss.wav" "$tmp/body.wav"

# 8. squelch tail: unkey burst, loud at the front, gone fast
"${sox[@]}" -n "${fmt[@]}" "$tmp/tail.wav" synth "$tail_s" whitenoise sinc 400-3000 gain "$tail_db" \
	fade q 0 "$tail_s" "$(awk -v t="$tail_s" 'BEGIN { printf "%.3f", t * 0.8 }')"

"${sox[@]}" "$tmp/body.wav" "$tmp/tail.wav" "$out" gain -n -1
