#!/usr/bin/env bash
# demo.sh: rebuild the dispatch radio demo end to end.
#   synthesize dispatch-traffic.tsv in one SpeechGenSynth batch -> radio chain on every line
#   -> montage, full montage, the call-sign lineup (the ear test) and an A/B pair
#
# Usage: demo.sh <Project.uproject> [out-dir]
#   out-dir defaults to <project>/Saved/SpeechGenOut/radio
#   UE_ROOT overrides the engine (default /Users/Shared/Epic Games/UE_5.8)
#   MONTAGE_LINES picks the montage lines by number prefix (default 01-14: first contacts through the missing-person broadcast)
# Montage gaps come from the script's fifth column (two seconds between transmissions), not from chance.
# The project needs SpeechGen enabled and the runtime installed.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
project=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=${2:-$(dirname "$project")/Saved/SpeechGenOut/radio}
ue=${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}
lines=${MONTAGE_LINES:-"01 02 03 04 05 06 07 08 09 10 11 12 13 14"}

mkdir -p "$out/clean" "$out/fx" "$out/ab"
# Renumbered scripts must not leave stale lines behind for the globs below.
rm -f "$out"/clean/*.wav "$out"/fx/*.wav
"$ue/Engine/Binaries/Mac/UnrealEditor-Cmd" "$project" -run=SpeechGenEditor.SpeechGenSynth \
	-Batch="$here/dispatch-traffic.tsv" -OutDir="$out/clean" -Timings="$out/timings.csv" \
	-unattended -nullrhi -nosplash -nopause -stdout > "$out/batch.log" 2>&1
grep -E "LogSpeechGenSynth" "$out/batch.log" | sed -E 's/^\[[^]]*\]\[[^]]*\]LogSpeechGenSynth: Display: //'

for file in "$out"/clean/*.wav; do
	"$here/radiofx.sh" "$file" "$out/fx/$(basename "$file")"
done

# Gap before each numbered line, keyed by its number prefix.
gap_for() {
	awk -F'\t' -v n="$1" '$0 !~ /^#/ && NF >= 5 && index($1, n "-") == 1 { print $5 }' "$here/dispatch-traffic.tsv"
}
montage_of() {
	local target=$1; shift
	local files=() gaps=() first=1 n
	for n in "$@"; do
		files+=("$out"/fx/"$n"-*.wav)
		if [[ $first -eq 0 ]]; then gaps+=("$(gap_for "$n")"); fi
		first=0
	done
	"$here/montage.sh" -L "${gaps[*]}" "$target" "${files[@]}"
}
montage_of "$out/montage.wav" $lines
montage_of "$out/montage-full.wav" $(ls "$out"/fx/[0-9]*.wav | xargs -n1 basename | cut -d- -f1) > /dev/null
"$here/montage.sh" -g 0.6,0.6 "$out/lineup.wav" "$out"/fx/lineup-*.wav > /dev/null

# A/B: the same line clean and on the radio, level-matched by RMS.
"$here/radiofx.sh" -c "$out/clean/03-echo.wav" "$out/ab/A-clean-03-echo.wav"
a_rms=$(sox "$out/ab/A-clean-03-echo.wav" -n stats 2>&1 | awk '/RMS lev dB/ { print $4 }')
b_rms=$(sox "$out/fx/03-echo.wav" -n stats 2>&1 | awk '/RMS lev dB/ { print $4 }')
sox -V1 "$out/fx/03-echo.wav" "$out/ab/B-radio-03-echo.wav" gain "$(awk -v a="$a_rms" -v b="$b_rms" 'BEGIN { printf "%.2f", a - b }')"
echo "A/B: $out/ab/"
