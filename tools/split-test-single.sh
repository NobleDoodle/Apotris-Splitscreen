#!/bin/bash
# Regression check: without --split the game is one process with one normal
# window and no player 2 folder. Same isolation as split-test.sh.
#   tools/split-test-single.sh OUTDIR [SAVE]
OUT="$(realpath -m "$1")"; SAVE="${2:-}"
BIN="${SPLIT_TEST_BIN:-$(realpath build-linux/Apotris)}"
rm -rf "$OUT"
mkdir -p "$OUT/home/.local/share/Apotris" "$OUT/run"
cc -shared -fPIC $(pkg-config --cflags sdl2) -o "$OUT/fake-pads.so" tools/split-fake-pads.c -ldl || exit 1
mkfifo "$OUT/pads"
exec 9<>"$OUT/pads"
chmod 700 "$OUT/run"
[ -n "$SAVE" ] && cp "$SAVE" "$OUT/home/.local/share/Apotris/Apotris.sav"
Xvfb :78 -screen 0 1920x1080x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill "$XVFB" 2>/dev/null' EXIT
sleep 1
(
    cd "$OUT/home" || exit 1
    unset WAYLAND_DISPLAY XDG_DATA_HOME
    export HOME="$OUT/home" XDG_RUNTIME_DIR="$OUT/run" DISPLAY=:78
    export SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
    export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x1209/0x7777
    [ -z "${SPLIT_TEST_BIN:-}" ] && export LD_PRELOAD="$OUT/fake-pads.so" FAKE_PADS_FIFO="$OUT/pads"
    exec "$BIN" >"$OUT/log.txt" 2>&1
) &
GAME=$!
sleep 7
export DISPLAY=:78
WIN="$(xdotool search --name '^Apotris' | head -1)"
xdotool getwindowgeometry "$WIN" | tail -1
xdotool windowfocus "$WIN"
import -window root "$OUT/boot.png"
for _ in 1 2; do
    xdotool keydown s; sleep 0.08
    xdotool keyup s; sleep 0.4
done
sleep 1
import -window root "$OUT/single.png"
# Any pad drives single player: the second pad (not the first one) moves the cursor.
for _ in 1 2; do
    echo "b 1 12 1" >&9; sleep 0.08
    echo "b 1 12 0" >&9; sleep 0.4
done
sleep 0.6
import -window root "$OUT/second-pad.png"
echo "split children: $(pgrep -xf 'Apotris --split-child .*' | wc -l)"
kill -TERM "$GAME"
sleep 1
kill -0 "$GAME" 2>/dev/null && echo "still running" || echo "exited"
ls "$OUT/home/.local/share"
