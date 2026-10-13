# MM7 reverse-engineering notes

Target: GOG `mm7.exe` v1.21 + GrayFace patch 2.5.7, sha256 `673c9805ab4a7b7440aa1be937d0a508ec029f94c7e42be750673a0f977c678c`.

- `mm7_addr_map.tsv` — function addresses/signatures from OpenEnroth's first commit (2016 decompile of a different MM7 build).
- `ghidra_funcs.txt` — function starts found by Ghidra 12.1.2 auto-analysis in the target exe.
- `realign.py` — maps 2016 addresses onto the target by piecewise-constant delta voting (`python3 realign.py`).
- `mm7_addr_map_local.tsv` — result: old addr, new addr (`?` = unmapped), votes (max 25), file, signature.
- `mm7_symbols_ghidra.txt` — 1467 names for Ghidra's built-in `ImportSymbolsScript.py`.

Spot-checked: `Render::DrawTerrainPolygon` = 0x4A264C (DrawPrimitive TRIANGLEFAN, FVF 0x1C4 = pre-transformed TL vertices).
Memory layouts: prefer GrayFace's MMExtension (`Scripts/Structs`), its MM7 addresses match this exe.
