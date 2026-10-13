#!/bin/sh
# Give keyboard focus to the MM7 window (KDE Wayland, via a one-shot KWin script).
tmp=$(mktemp -d)
cat > "$tmp/focus.js" <<'JS'
for (const w of workspace.windowList())
  if (w.caption.indexOf("Might and Magic") >= 0) workspace.activeWindow = w;
JS
name=mm7focus$$
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$tmp/focus.js" $name)
qdbus6 org.kde.KWin /Scripting/Script$id org.kde.kwin.Script.run
sleep 0.3
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript $name >/dev/null
rm -rf "$tmp"
