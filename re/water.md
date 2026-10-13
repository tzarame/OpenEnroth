# Water rendering in vanilla mm7.exe (GOG 1.21 + GrayFace 2.5.7)

Sources: Ghidra decompile of the target exe, cross-checked against OpenEnroth's 2016 decompile (`_deleted.cpp`, `Render.h`).

## Call chain (outdoor terrain)

- `Render::DrawTerrainD3D` @ 0x480352 — walks visible terrain grid cells, builds one `Polygon` per cell
  in the array at 0x77EA88 (stride 0x10C, count in 0x6BDF1C), then calls `DrawTerrainPolygon` per tile.
- `Render::DrawTerrainPolygon` @ 0x4A264C, `__thiscall(Render*, uint nVerts, Polygon*, IDirect3DTexture2*, bool transparent, bool clampAtBorder)`.
  Converts the soft vertex array into D3DTLVERTEX (FVF 0x1C4) at 0x77E448 and calls
  `IDirect3DDevice3::DrawPrimitive(D3DPT_TRIANGLEFAN=6, 0x1C4, verts, n, 0x10)`, plus lightmap/fog passes.
  Prologue `55 8B EC 51 53` = 5 whole bytes → clean JMP detour site.

## Identifying water

`Polygon` (from 2016 Render.h; offsets verified against the decompile):

| off  | field                              |
|------|------------------------------------|
| 0x14 | uNumVertices                       |
| 0x30 | i16 flags = 0x8010 \| tile attributes (`GetSomeOtherTileInfo`) |
| 0x3C | Texture* pTexture                  |
| 0x4C | u16 uTileBitmapID (`GetTileTexture`) |
| 0x5C | i8 terrain_grid_z (row)            |
| 0x5D | i8 terrain_grid_x (col)            |

Water tile ⇔ `flags & 2`. Vanilla then swaps the texture for the animated HD water frame when
`uTileBitmapID == hd_water_tile_id` (`pHDWaterBitmapIDs[hd_water_current_frame]`; GrayFace exposes
frame count/delay as `HDWTRCount`/`HDWTRDelay` in mm7.ini).

Water inside BSP models (fountains, moats) goes through `DrawBuildingsD3D` @ 0x478411 → `DrawPolygon` @ 0x4A2050
with an `ODMFace*`; fluid faces are flagged in the face attributes (to be checked).

## Data available at DrawTerrainPolygon time

Soft vertices: `RenderVertexSoft[]` at **0x50AA90**, stride 0x30:
`vWorldPosition xyz (0x00)`, `vWorldViewPosition xyz (0x0C)`, `vWorldViewProjX/Y (0x18)`, `_rhw (0x20)`, `u, v (0x24)`, `flt_2C`.

So the hook sees **world-space** positions of every water polygon, not only screen vertices.
Outdoor water is flat (terrain water tiles share one height per map), which makes a planar mirror feasible.

Camera: party pos/yaw/pitch from MMExtension `Party` (0xACD4EC: X,Y,Z,Direction 0..2047 (0=E,512=N), LookAngle);
exact view/projection still to locate (`IndoorCameraD3D` functions in the address map).

## Plan for the water hook (ddraw.dll proxy)

1. Detour `DrawTerrainPolygon`; if `poly->flags & 2`, record the polygon (world verts + screen verts) instead of / in addition to drawing it.
2. After the 3D scene, draw recorded water polygons with our own shader (reflection + fresnel + normal-mapped ripples),
   using the recorded screen verts so depth/occlusion stays consistent with the game's z-buffer.
3. Reflection source: start screen-space (needs only the frame), upgrade to a mirrored render pass later.

## Gotchas found so far

- Game pauses on focus loss via WndProc 0x463828 WM_ACTIVATEAPP branch at 0x463BAA (sets 0x100 in 0x6BE1E4,
  `Timer::Pause`, Game_Loop then sleeps in `WaitMessage`). Under Wine the branch is taken even with wParam=1.
  GrayFace's patch already detours the WndProc entry.
- Sending UI messages directly (e.g. `UIMSG_BuyInShop_Identify_Repair`=81) without an active character crashes in
  `HouseUI_CheckIfPlayerCanInteract` @ 0x4B16E7 (null player). Select a character first.
