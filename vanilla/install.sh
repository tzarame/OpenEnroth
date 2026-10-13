#!/bin/sh
# Install / remove the vanilla MM7 graphics chain: dxwrapper (D3D7->D3D9) + ReShade (d3d9.dll) + mm7fx add-on.
# Usage: install.sh [install|remove|logs]
set -e
G="${MM7_DIR:-$HOME/Games/Heroic/Might and Magic 7}"
C="$HOME/.config/heroic/GamesConfig/1207658916.json"
here=$(cd "$(dirname "$0")" && pwd)
set_env() { jq --argjson v "$1" '."1207658916".enviromentOptions = $v' "$C" > "$C.tmp" && mv "$C.tmp" "$C"; }
case "${1:-install}" in
install)
	cp "$HOME/Tools/dxwrapper/Stub/ddraw.dll" "$HOME/Tools/dxwrapper/dxwrapper.dll" "$G/"
	[ -f "$G/dxwrapper.ini" ] || sed -E 's/^(Dd7to9 +)= 0/\1= 1/' "$HOME/Tools/dxwrapper/dxwrapper.ini" > "$G/dxwrapper.ini"
	cp "$HOME/Tools/reshade/6.8.0_Addon/ReShade32.dll" "$G/d3d9.dll"
	cp "$here/mm7fx/mm7fx.addon32" "$G/"
	set_env '[{"key":"WINEDLLOVERRIDES","value":"ddraw=n,b;d3d9=n,b"}]'
	echo "installed into $G (restart Heroic if it doesn't pick up WINEDLLOVERRIDES)";;
remove)
	rm -f "$G/ddraw.dll" "$G/dxwrapper.dll" "$G/dxwrapper.ini" "$G/d3d9.dll" "$G/mm7fx.addon32" "$G/ReShade.ini"
	set_env '[]'
	echo "removed";;
logs)
	for f in "$G"/dxwrapper*.log "$G"/ReShade.log; do [ -f "$f" ] && { echo "== $f"; tail -n 40 "$f"; }; done;;
esac
