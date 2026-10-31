#!/bin/bash
# The audio battery on Linux: runs the AppImage in each variant, records the
# title music by loopback from the default output (PipeWire or PulseAudio)
# and analyzes it with analyze.py. After the title it waits for Enter, so the
# tester can open the main menu (START), and records the menu too.
#
# Usage: battery-linux.sh <AppImage> <output dir> [variant...]
# Variants: default | sdl-2400 (SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2400)
#           | quantum-2400 (PIPEWIRE_QUANTUM=2400/48000, the block of issue #86)
# Needs parecord (pulseaudio-utils or pipewire-pulse) or pw-record, and python3
# with numpy. The game takes the screen and plays audio: nothing else may play.
set -euo pipefail
image=$(realpath "$1")
out=$(realpath -m "$2")
shift 2
variants=("$@")
[ ${#variants[@]} -eq 0 ] && variants=(default sdl-2400 quantum-2400)
here=$(dirname "$(realpath "$0")")
logs="${XDG_DATA_HOME:-$HOME/.local/share}/narutorise/logs"
mkdir -p "$out"

record() {  # seconds file
  local sink
  sink=$(pactl get-default-sink 2>/dev/null || true)
  if command -v parecord >/dev/null && [ -n "$sink" ]; then
    timeout -s INT "$1" parecord --device="$sink.monitor" --file-format=wav --format=s16le --rate=48000 \
      --channels=2 "$2" || true
  else
    timeout -s INT "$1" pw-record -P '{ stream.capture.sink = true }' --format s16 --rate 48000 --channels 2 \
      "$2" || true
  fi
}

for variant in "${variants[@]}"; do
  case "$variant" in
    default) env=() ;;
    sdl-2400) env=(SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2400) ;;
    quantum-2400) env=(PIPEWIRE_QUANTUM=2400/48000) ;;
    *) echo "unknown variant $variant"; exit 2 ;;
  esac
  echo "== $variant: starting the game"
  env "${env[@]}" "$image" --audio_layer=native >"$out/$variant.stdout" 2>&1 &
  game=$!
  sleep 40
  echo "== $variant: recording 30 s of the title"
  record 30 "$out/$variant-title.wav"
  read -r -p "Open the main menu (START), then press Enter here to record it (s + Enter skips): " answer
  if [ "$answer" != "s" ]; then
    sleep 3
    echo "== $variant: recording 30 s of the main menu"
    record 30 "$out/$variant-menu.wav"
  fi
  kill "$game" 2>/dev/null || true
  wait "$game" 2>/dev/null || true
  latest=$(ls -t "$logs"/*.log 2>/dev/null | head -1 || true)
  [ -n "$latest" ] && cp "$latest" "$out/$variant.log"
done

files=()
for f in "$out"/*.wav; do [ -s "$f" ] && files+=("$f"); done
python3 -I "$here/analyze.py" "${files[@]}" | tee "$out/analysis.txt"
