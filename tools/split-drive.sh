#!/bin/bash
# Send input to a test session held open by split-test.sh (SPLIT_TEST_HOLD=N).
#   tools/split-drive.sh OUTDIR STEP...
# Steps: pad:PLAYER:BUTTON[:COUNT]  key:KEY[:COUNT]  shot:NAME  wait:SECONDS
# Buttons: 0=A 1=B 11=up 12=down 13=left 14=right 6=start
OUT="$1"; shift
export DISPLAY=:77
exec 9<>"$OUT/pads"
for step in "$@"; do
    IFS=: read -r kind a b c <<<"$step"
    case "$kind" in
    pad)
        for ((i = 0; i < ${c:-1}; i++)); do
            echo "b $a $b 1" >&9; sleep 0.08
            echo "b $a $b 0" >&9; sleep 0.25
        done
        ;;
    key)
        for ((i = 0; i < ${b:-1}; i++)); do
            xdotool keydown "$a"; sleep 0.08
            xdotool keyup "$a"; sleep 0.25
        done
        ;;
    shot) import -window root "$OUT/$a.png" ;;
    wait) sleep "$a" ;;
    *) echo "unknown step: $step" >&2; exit 2 ;;
    esac
done
