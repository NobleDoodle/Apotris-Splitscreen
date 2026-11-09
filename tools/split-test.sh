#!/bin/bash
# Run split screen on a private X server and capture what it draws. Uses a
# throwaway HOME and no audio device, so the real saves and speakers are safe.
#
#   tools/split-test.sh OUTDIR [SAVE] [WxH]
#
# SAVE seeds player 1's save. Real pads are hidden from SDL.
set -u
OUT="$(realpath -m "$1")"; SAVE="${2:-}"; GEOM="${3:-1920x1080}"
BIN="$(realpath "${SPLIT_TEST_BIN:-build-linux/Apotris}")"
SCENARIO="${SPLIT_TEST_SCENARIO:-default}"
# The toggle scenario starts as one player and switches split screen on from
# Multi Battle; the others start with --split.
ARGS=--split
case "$SCENARIO" in toggle | keys) ARGS= ;; esac
DISP=:77
FAIL=0
REAL_SAVE="$HOME/.local/share/Apotris/Apotris.sav"
REAL_P2="$HOME/.local/share/Apotris-p2/Apotris.sav"
REAL_BEFORE="$(sha256sum "$REAL_SAVE" 2>/dev/null | cut -d' ' -f1)"
REAL_P2_BEFORE="$(sha256sum "$REAL_P2" 2>/dev/null | cut -d' ' -f1)"

rm -rf "$OUT"
mkdir -p "$OUT/home/.local/share/Apotris" "$OUT/run"
chmod 700 "$OUT/run"
[ -n "$SAVE" ] && cp "$SAVE" "$OUT/home/.local/share/Apotris/Apotris.sav"
P1="$OUT/home/.local/share/Apotris/Apotris.sav"
P2="$OUT/home/.local/share/Apotris-p2/Apotris.sav"

cc -shared -fPIC $(pkg-config --cflags sdl2) -o "$OUT/fake-pads.so" tools/split-fake-pads.c -ldl || exit 1
FIFO="$OUT/pads"
mkfifo "$FIFO"
exec 9<>"$FIFO"

Xvfb "$DISP" -screen 0 "${GEOM}x24" >/dev/null 2>&1 &
XVFB=$!
trap 'kill "$XVFB" 2>/dev/null' EXIT
sleep 1

shot() { DISPLAY=$DISP import -window root "$OUT/$1.png"; }
pad() { # pad PLAYER BUTTON [COUNT]   (0 is P1's pad, 1 is P2's)
    local i
    for ((i = 0; i < ${3:-1}; i++)); do
        echo "b $1 $2 1" >&9; sleep 0.08
        echo "b $1 $2 0" >&9; sleep 0.25
    done
}
A=0 B=1 SELECT=4 START=6 UP=11 DOWN=12 LEFT=13 RIGHT=14
hash() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
press() { # press KEY [COUNT]
    local i
    for ((i = 0; i < ${2:-1}; i++)); do
        DISPLAY=$DISP xdotool keydown "$1"; sleep 0.08
        DISPLAY=$DISP xdotool keyup "$1"; sleep 0.25
    done
}
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAIL=1; fi; }
children() { pgrep -xf "Apotris --split-child .*" | wc -l; }

(
    cd "$OUT/home" || exit 1
    unset WAYLAND_DISPLAY XDG_DATA_HOME
    export HOME="$OUT/home" XDG_RUNTIME_DIR="$OUT/run" DISPLAY=$DISP
    export SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER="${SPLIT_TEST_AUDIO:-dummy}"
    export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x1209/0x7777
    export LD_PRELOAD="$OUT/fake-pads.so" FAKE_PADS_FIFO="$FIFO"
    [ -n "${SPLIT_TEST_NO_DISPLAY:-}" ] && unset DISPLAY SDL_VIDEODRIVER   # like a start over SSH
    # Offline: the shim refuses every IPv4/IPv6 socket in both processes.
    # (A network namespace would be cleaner, but Ubuntu blocks unprivileged ones.)
    [ -n "${SPLIT_TEST_OFFLINE:-}" ] && export FAKE_NO_NETWORK=1
    if [ -n "${SPLIT_TEST_GDB:-}" ]; then   # backtrace in log.txt if player 1 crashes
        exec gdb -q -batch -ex 'set startup-with-shell off' -ex run -ex bt \
            --args "$BIN" $ARGS >"$OUT/log.txt" 2>&1
    fi
    exec "$BIN" $ARGS >"$OUT/log.txt" 2>&1
) &
GAME=$!
sleep 6

WIN="$(DISPLAY=$DISP xdotool search --name '^Apotris' | head -1)"
check "window exists" '[ -n "$WIN" ]'
DISPLAY=$DISP xdotool windowsize "$WIN" "${GEOM%x*}" "${GEOM#*x}"
DISPLAY=$DISP xdotool windowmove "$WIN" 0 0
DISPLAY=$DISP xdotool windowfocus "$WIN"
sleep 1
if [ -n "$ARGS" ]; then
    check "player 2 process running" '[ "$(children)" = 1 ]'
else
    check "no player 2 before split screen is on" '[ "$(children)" = 0 ]'
fi
check "player 2 opened no window" '[ "$(DISPLAY=$DISP xdotool search --name "^Apotris" | wc -l)" = 1 ]'
shot 01-boot
if [ -n "${SPLIT_TEST_EXPLORE:-}" ]; then   # hold at boot for split-drive.sh
    sleep "$SPLIT_TEST_EXPLORE"
    kill -TERM "$GAME"
    exit 0
fi
if [ -n "${SPLIT_TEST_IDLE:-}" ]; then   # leave both title screens alone
    sleep "$SPLIT_TEST_IDLE"
    shot 00-idle
fi

lobby_checks() { # both halves joined one battle
    check "both halves joined the battle" '[ "$(grep -c "battle: paired" "$OUT/log.txt")" -ge 2 ]'
    check "both sides share one seed" '[ "$(grep -o "seed [0-9]*" "$OUT/log.txt" | sort -u | wc -l)" = 1 ]'
}

ready_up() { # P1 hosts and picks first-to, P2 readies; the round starts
    pad 0 $A
    pad 0 $A
    pad 1 $A
    sleep 5
}

topout_p2() { # P2 holds soft drop until it tops out
    # A held button is one press, so nothing spills into the end screen and
    # both cursors stay on Play Again.
    echo "b 1 $DOWN 1" >&9
    sleep 30
    echo "b 1 $DOWN 0" >&9
    sleep 2
}

# --split: both halves start in the battle's lobby. Input stays on its own
# side; P2 tops out; P1 leaving for the main menu closes P2.
default_scenario() {
sleep 1
shot 02-lobby
lobby_checks
ready_up
pad 0 $LEFT 2        # each pad moves only its own piece
pad 1 $RIGHT 2
sleep 0.3
shot 03-battle
press F8             # the keyboard to player 2, held long enough to repeat
DISPLAY=$DISP xset r on
DISPLAY=$DISP xdotool keydown Right
sleep 1.2
DISPLAY=$DISP xdotool keyup Right
sleep 0.2
shot 04-held-key     # player 1's piece must not have slid
press F8
topout_p2
shot 05-result
pad 0 $DOWN          # Play Again -> Main Menu
pad 0 $A
sleep 3
shot 06-p1-menu
check "player 1 leaving for the menu closes player 2" '[ "$(children)" = 0 ]'
check "back to one screen" 'grep -q "split screen: off" "$OUT/log.txt"'
check "player 2 saved when closed" '[ -s "$P2" ]'
}

# Round 2: both pick Play Again; it starts on the next seed, the same on both
# sides, and split screen stays on.
battle_scenario() {
sleep 1
lobby_checks
if [ -n "${SPLIT_TEST_BOTS:-}" ]; then
    pad 0 $SELECT    # debug builds: SELECT in the lobby hands a side to the bot
    pad 1 $SELECT
fi
ready_up
shot b4-battle
if [ -n "${SPLIT_TEST_BOTS:-}" ]; then
    for ((t = 0; t < SPLIT_TEST_BOTS; t += 10)); do
        sleep 10
        shot "b4-bots-$t"
    done
    return
fi
topout_p2
shot b5-result
pad 0 $A
pad 1 $A
sleep 1
shot b6-again
sleep 5
shot b7-round2
check "split screen stays on for Play Again" '[ "$(children)" = 1 ]'
}

# Start as one player. Multi Battle > Split Screen opens player 2 already in
# the battle; leaving the lobby closes it; then the same again.
toggle_scenario() {
pad 0 $A             # title -> main menu
pad 0 $A             # Play
sleep 0.5
pad 0 $DOWN 11       # Multi Battle
pad 0 $A
sleep 1.5
shot t1-multi
pad 1 $DOWN          # every pad is player 1's before split screen
pad 0 $A             # Split Screen
sleep 5
shot t2-lobby
check "Split Screen starts player 2" '[ "$(children)" = 1 ]'
lobby_checks
pad 0 $B             # player 1 leaves the lobby
sleep 3
shot t3-left
check "leaving the battle closes player 2" '[ "$(children)" = 0 ]'
check "player 2 saved when closed" '[ -s "$P2" ]'
pad 0 $A             # back on the Play list at Multi Battle
sleep 1.5
pad 0 $DOWN
pad 0 $A             # Split Screen again
sleep 5
shot t4-again
check "Split Screen again starts player 2" '[ "$(children)" = 1 ]'
check "both halves joined again" '[ "$(grep -c "battle: paired" "$OUT/log.txt")" -ge 4 ]'
}

# Keyboard only, for builds with SDL linked in statically (a release build),
# which the fake pads cannot reach: Multi Battle > Split Screen, then leave.
keys_scenario() {
press Return         # title -> main menu
press Return         # Play
sleep 0.5
press s 11           # Multi Battle
press Return
sleep 1.5
press s
press Return         # Split Screen
sleep 5
shot k1-lobby
check "Split Screen starts player 2" '[ "$(children)" = 1 ]'
lobby_checks
press BackSpace      # player 1 leaves the lobby
sleep 3
shot k2-left
check "leaving the battle closes player 2" '[ "$(children)" = 0 ]'
}

case "$SCENARIO" in
battle) battle_scenario ;;
toggle) toggle_scenario ;;
keys) keys_scenario ;;
*) default_scenario ;;
esac

[ -n "${SPLIT_TEST_HOLD:-}" ] && sleep "$SPLIT_TEST_HOLD"

# SPLIT_TEST_QUIT picks how the session ends: term (SIGTERM, what a window manager's quit sends),
# window (the close button), or child (player 2 dies first).
case "${SPLIT_TEST_QUIT:-term}" in
term)
    kill -TERM "$GAME"
    ;;
window)
    echo "q 0 0 0" >&9   # xdotool windowquit needs a window manager
    ;;
child)
    pkill -TERM -xf "Apotris --split-child .*"
    sleep 1.5
    check "player 1 survives player 2 dying" 'kill -0 "$GAME" 2>/dev/null'
    pad 0 $B             # out of the lobby, to a menu
    sleep 2
    check "player 1 went back to one screen" 'grep -q "split screen: off" "$OUT/log.txt"'
    shot 09-p2-gone
    kill -TERM "$GAME"
    ;;
esac
for ((waited = 0; waited < 150; waited++)); do
    kill -0 "$GAME" 2>/dev/null || break
    sleep 0.1
done
echo "     player 1 took $((waited / 10)).$((waited % 10))s to exit"
sleep 0.5
check "player 1 exited" '! kill -0 "$GAME" 2>/dev/null'
check "no player 2 left behind" '[ "$(children)" = 0 ]'
check "real save untouched" '[ "$(hash "$REAL_SAVE")" = "$REAL_BEFORE" ]'
check "real player 2 save untouched" '[ "$(hash "$REAL_P2")" = "$REAL_P2_BEFORE" ]'

if [ -n "${SPLIT_TEST_OFFLINE:-}" ]; then
    echo "     network sockets the game tried to open: $(grep -c 'refused a network socket' "$OUT/log.txt")"
fi
grep -iE "segmentation|error|failed" "$OUT/log.txt" | head -5
exit "$FAIL"
