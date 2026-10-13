#!/bin/sh
# A/B screenshots of the same frame: mode $1 vs mode $2 (default 0=effect vs 1=vanilla); prints differing pixel share.
G="${MM7_DIR:-$HOME/Games/Heroic/Might and Magic 7}"; d=$(dirname "$0"); a=${1:-0}; b=${2:-1}
echo $a > "$G/mm7fx_mode.txt"; sleep 1.2; "$d/mm7shot.sh" /tmp/claude-1000/ab_$a.png >/dev/null
echo $b > "$G/mm7fx_mode.txt"; sleep 1.2; "$d/mm7shot.sh" /tmp/claude-1000/ab_$b.png >/dev/null
echo $a > "$G/mm7fx_mode.txt"
magick compare -metric AE -fuzz 4% /tmp/claude-1000/ab_$a.png /tmp/claude-1000/ab_$b.png /tmp/claude-1000/ab_diff.png 2>&1 | awk '{printf "differing pixels: %s (%.1f%%)\n", $1, $2*100}'
