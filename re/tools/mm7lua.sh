#!/bin/sh
# Run Lua inside MM7 via MMExtension + ClaudeBridge.lua. Usage: mm7lua.sh 'return Party.X'
d=~/Tools/mm7bridge; rm -f "$d/out.txt"; printf '%s' "$1" > "$d/cmd.lua.tmp" && mv "$d/cmd.lua.tmp" "$d/cmd.lua"
for i in $(seq 1 600); do [ -f "$d/out.txt" ] && { cat "$d/out.txt"; exit 0; }; sleep 0.1; done
echo "timeout (game not running, paused or unfocused?)"; exit 1
