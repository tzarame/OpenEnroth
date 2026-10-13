#!/bin/sh
# Screenshot only the MM7 window (KDE Wayland): KWin script -> geometry, spectacle full shot -> crop -> delete full shot.
# Usage: mm7shot.sh [out.png]
set -e
[ "${MM7_RAISE:-0}" = 1 ] && { "$(dirname "$0")/mm7focus.sh"; sleep 0.4; }  # MM7_RAISE=1: raise the game window (steals focus)
out=${1:-/tmp/claude-1000/mm7.png}
tmp=$(mktemp -d)
cat > "$tmp/geo.js" <<'JS'
for (const w of workspace.windowList())
  if (w.caption.indexOf("Might and Magic") >= 0) {
    const g = w.frameGeometry;
    print("MM7GEO " + Math.round(g.x) + " " + Math.round(g.y) + " " + Math.round(g.width) + " " + Math.round(g.height));
  }
JS
name=mm7geo$$
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$tmp/geo.js" $name)
qdbus6 org.kde.KWin /Scripting/Script$id org.kde.kwin.Script.run
sleep 0.5
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript $name >/dev/null
geo=$(journalctl --user -n 200 --since '-30s' -o cat | grep MM7GEO | tail -1)
[ -n "$geo" ] || { echo "MM7 window not found"; rm -rf "$tmp"; exit 1; }
set -- $geo
spectacle -b -n -f -o "$tmp/full.png" >/dev/null 2>&1
magick "$tmp/full.png" -crop "${4}x${5}+${2}+${3}" +repage "$out"
rm -rf "$tmp"
echo "$out"
