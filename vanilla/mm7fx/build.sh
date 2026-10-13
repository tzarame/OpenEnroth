#!/bin/sh
# Cross-compile the 32-bit ReShade add-on with mingw-w64. Output: mm7fx.addon32
set -e
cd "$(dirname "$0")"
RESHADE_INC=${RESHADE_INC:-$HOME/Tools/reshade/reshade-src/include}
i686-w64-mingw32-g++ -std=c++20 -O2 -s -Wno-template-body -Wno-attributes -shared -static -static-libgcc -static-libstdc++ \
  -Icompat -I"$RESHADE_INC" -o mm7fx.addon32 mm7fx.cpp -Wl,--enable-stdcall-fixup
echo "built $(pwd)/mm7fx.addon32"
