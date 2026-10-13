# Rendering plan for vanilla mm7.exe

Goal: modern effects (water reflections, sun shadows) in the original game, Windows-native, run on Linux via Proton.
Hardware now: Intel UHD 770 (no ray tracing) → shadow maps, no RT.

## Chain

```
mm7.exe ─D3D7─▶ ddraw.dll (dxwrapper, Dd7to9) ─D3D9─▶ d3d9.dll (ReShade + our addon) ─▶ system d3d9 (DXVK on Proton / native on Windows)
                                    ▲
             our hooks in mm7.exe (MMExtension Lua / own DLL): world-space data — water polys, camera, sun, models
```

Why:
- dxwrapper `Dd7to9` (v1.9.8700.25, ~/Tools/dxwrapper) already translates DirectDraw/D3D7 → D3D9. MM7 is not on its
  compatibility list; MM8 (same engine) is listed as "Unknown". First step = test it.
- Once the game is D3D9, ReShade gives a shader framework with depth-buffer access, and its addon API (C++) lets us
  intercept draws and add our own passes — no need to write a D3D7 translator ourselves.
- The game's TL vertices are screen-space, so world-space info comes from our hooks in mm7.exe
  (`DrawTerrainPolygon` water flag + world verts at 0x50AA90, `Map.Models`, `Party`, game time).

## Steps

1. Test dxwrapper: copy `ddraw.dll`, `dxwrapper.dll`, `dxwrapper.ini` (Dd7to9=1) into the game dir,
   Heroic env `WINEDLLOVERRIDES=ddraw=n,b`. Check: menus, 3D, movies, GrayFace HD/windowed options, Alt+Tab.
   Known knobs if broken: FullscreenWindowMode, D3d9to9Ex, DdrawEmulateSurface, DdrawEmulateLock.
2. Add ReShade (D3D9) on top; confirm depth buffer is visible (generic depth addon).
3. Water: hook `DrawTerrainPolygon`; tag water draws (flags & 2) so the addon can recognize them
   (e.g. via a marker render-state or by texture), draw them with our shader (SSR → later planar mirror).
4. Shadows: blob shadows for billboards → sun shadow map from static geometry (models + terrain + tree cards),
   refreshed every few game minutes.
