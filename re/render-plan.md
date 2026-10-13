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

## Water pass v2 (design, after the chain is confirmed working)

1. Capture: in `on_draw`, while inside `DrawTerrainPolygon` with `flags & 2`, skip the draw and copy its TL vertices
   (game's D3DTLVERTEX array at 0x77E448, stride 0x20: x,y,z,rhw,diffuse,specular,u,v) + the bound water texture.
   Only the first draw per polygon (base texture); the following lightmap/fog draws of the same polygon are dropped.
2. Draw point: detour `Render::DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene` @ 0x4A1F71
   (`__thiscall`, no args; first insn `dec [ecx+0x4010C]` = 6 bytes). When `[ecx+0x4010C] == 1` the scene is about to
   end: terrain/buildings/sky are drawn, billboards/effects/UI are not. Draw captured water there, then call original.
3. Shader: ps_3_0 compiled at runtime with `D3DCompile` from d3dcompiler_47.dll (native on Windows, vkd3d in Proton).
   XYZRHW vertices + pixel shader is legal in D3D9. Inputs: copy of the back buffer (scene so far), water texture,
   time, camera pitch. Reflection: mirror the screen around the horizon line + animated ripple offset;
   Fresnel from distance to horizon; mix with the original water texture.
4. Later: replace the screen-space mirror with a real mirrored pass using `Map.Models` / terrain geometry.
