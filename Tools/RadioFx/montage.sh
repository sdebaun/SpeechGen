#!/usr/bin/env bash
# montage.sh: stitch radio transmissions into one stretch of traffic, with a random silent gap between keyups.
#
# Usage: montage.sh [-g min,max] [-s seed] [-L "gap gap ..."] out.wav in1.wav in2.wav ...
#   -g   gap range in seconds between transmissions (default 0.3,1.2); the squelch is closed, so gaps are silence
#   -s   seed for the gap lengths (default 7), so a montage rebuilds identically
#   -L   explicit gaps, one per join, instead of random ones
# Prints each gap as it goes. Requires sox.
set -euo pipefail

gaps=0.3,1.2
seed=7
explicit=""
while getopts "g:s:L:" opt; do
	case $opt in
		g) gaps=$OPTARG ;;
		s) seed=$OPTARG ;;
		L) explicit=$OPTARG ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [[ $# -lt 2 ]]; then
	echo "usage: $0 [-g min,max] [-s seed] out.wav in1.wav [in2.wav ...]" >&2
	exit 2
fi
out=$1
shift

gap_min=${gaps%,*}
gap_max=${gaps#*,}
rate=$(soxi -r "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# One gap per join, drawn up front from the seed. (No mapfile: macOS ships bash 3.2.)
if [[ -n "$explicit" ]]; then
	lengths=($explicit)
else
	lengths=($(awk -v n=$(($# - 1)) -v lo="$gap_min" -v hi="$gap_max" -v s="$seed" \
		'BEGIN { srand(s); for (i = 0; i < n; i++) printf "%.2f\n", lo + rand() * (hi - lo) }'))
fi

parts=()
index=0
for file in "$@"; do
	parts+=("$file")
	if [[ $index -lt ${#lengths[@]} ]]; then
		gap="$tmp/gap-$index.wav"
		# -D: no dither, so the gap is true digital silence and the montage rebuilds byte for byte
		sox -V1 -D -n -r "$rate" -c 1 -b 16 "$gap" trim 0 "${lengths[$index]}"
		parts+=("$gap")
		echo "gap after $(basename "$file"): ${lengths[$index]} s"
	fi
	index=$((index + 1))
done

sox -V1 "${parts[@]}" "$out"
echo "$out: $(soxi -D "$out") s"
