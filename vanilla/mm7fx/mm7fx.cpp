// mm7fx: ReShade add-on for vanilla MM7 (D3D7 -> dxwrapper Dd7to9 -> D3D9 -> ReShade).
// Target: GOG mm7.exe 1.21 + GrayFace patch 2.5.7. Addresses: ../../re/water.md
//
// v1: detours Render::DrawTerrainPolygon so draw events can tell which terrain tile is being drawn.
//     F10 cycles: normal -> hide water tiles (Polygon::flags & 2) -> hide all terrain (proof of detection/skipping).
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <windows.h>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "mm7fx";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Might and Magic VII graphics experiments.";

// ---- game side -------------------------------------------------------------------------------

namespace mm7 {
constexpr uintptr_t DrawTerrainPolygon = 0x4A264C;      // __thiscall(Render*, n, Polygon*, tex, transparent, clamp)
constexpr uint8_t DrawTerrainPolygonPrologue[5] = {0x55, 0x8B, 0xEC, 0x51, 0x53};
constexpr uintptr_t SoftVertices = 0x50AA90;            // RenderVertexSoft[], stride 0x30, world pos at +0
constexpr size_t PolygonFlags = 0x30;                   // i16
constexpr uint16_t TileWater = 2;
}

using DrawTerrainPolygonFn = void(__fastcall *)(void *self, void *edx, uint32_t n, uint8_t *poly, void *tex, int transparent, int clamp);
static DrawTerrainPolygonFn s_orig_draw_terrain_polygon = nullptr;

static uint8_t *s_current_poly = nullptr; // set only while the game is inside DrawTerrainPolygon

static void __fastcall hook_draw_terrain_polygon(void *self, void *edx, uint32_t n, uint8_t *poly, void *tex, int transparent, int clamp)
{
	s_current_poly = poly;
	s_orig_draw_terrain_polygon(self, edx, n, poly, tex, transparent, clamp);
	s_current_poly = nullptr;
}

// 5-byte JMP detour with a trampoline holding the original 5 prologue bytes (whole instructions).
static bool install_detour(uintptr_t target, const uint8_t (&expected)[5], void *hook, void **original)
{
	if (std::memcmp(reinterpret_cast<void *>(target), expected, 5) != 0)
		return false; // someone else (e.g. GrayFace) already patched it, or a different exe

	auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!tramp)
		return false;
	std::memcpy(tramp, expected, 5);
	tramp[5] = 0xE9;
	*reinterpret_cast<int32_t *>(tramp + 6) = static_cast<int32_t>(target + 5 - reinterpret_cast<uintptr_t>(tramp + 10));
	*original = tramp;

	DWORD old;
	VirtualProtect(reinterpret_cast<void *>(target), 5, PAGE_EXECUTE_READWRITE, &old);
	auto *p = reinterpret_cast<uint8_t *>(target);
	p[0] = 0xE9;
	*reinterpret_cast<int32_t *>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (target + 5));
	VirtualProtect(reinterpret_cast<void *>(target), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(target), 5);
	return true;
}

static bool drawing_water()
{
	return s_current_poly && (*reinterpret_cast<int16_t *>(s_current_poly + mm7::PolygonFlags) & mm7::TileWater);
}

// ---- render side -----------------------------------------------------------------------------

static std::atomic<uint32_t> s_draws{0}, s_water_draws{0}, s_terrain_draws{0}, s_skipped{0};
enum class Hide { None, Water, Terrain };
static Hide s_hide = Hide::None;
static bool s_f10_was_down = false;
static uint32_t s_frame = 0;
static DWORD s_last_log = 0;

static bool on_draw(command_list *, uint32_t, uint32_t, uint32_t, uint32_t)
{
	++s_draws;
	if (!s_current_poly)
		return false;
	++s_terrain_draws;
	const bool water = drawing_water();
	if (water)
		++s_water_draws;
	const bool skip = s_hide == Hide::Terrain || (s_hide == Hide::Water && water);
	if (skip)
		++s_skipped;
	return skip; // true = skip this draw
}

static bool on_draw_indexed(command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	return on_draw(nullptr, 0, 0, 0, 0);
}

// ABI note: this add-on is built with MinGW GCC, ReShade with MSVC. MSVC returns user-defined types from member
// functions through a hidden pointer, GCC (for 8-byte structs) in EDX:EAX. Never call ReShade methods that return
// structs (get_back_buffer, get_resource_desc, get_resource_from_view, ...) directly; wrap them with an explicit
// __thiscall vtable call taking the result pointer.
static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	++s_frame;
	const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
	if (f10 && !s_f10_was_down) {
		s_hide = static_cast<Hide>((static_cast<int>(s_hide) + 1) % 3);
		static const char *const names[] = {"mm7fx: mode = normal", "mm7fx: mode = hide water", "mm7fx: mode = hide all terrain"};
		reshade::log::message(reshade::log::level::info, names[static_cast<int>(s_hide)]);
	}
	s_f10_was_down = f10;

	const DWORD now = GetTickCount();
	if (now - s_last_log >= 5000) {
		s_last_log = now;
		const float *v0 = reinterpret_cast<const float *>(mm7::SoftVertices);
		char buf[256];
		std::snprintf(buf, sizeof(buf), "mm7fx: frame %u: %u draws, %u terrain, %u water, %u skipped, mode %d, soft vertex[0] world (%.0f, %.0f, %.0f)",
			s_frame, s_draws.load(), s_terrain_draws.load(), s_water_draws.load(), s_skipped.load(), static_cast<int>(s_hide), v0[0], v0[1], v0[2]);
		reshade::log::message(reshade::log::level::info, buf);
	}
	s_draws = 0;
	s_water_draws = 0;
	s_terrain_draws = 0;
	s_skipped = 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::present>(on_present);
		if (install_detour(mm7::DrawTerrainPolygon, mm7::DrawTerrainPolygonPrologue,
				reinterpret_cast<void *>(hook_draw_terrain_polygon), reinterpret_cast<void **>(&s_orig_draw_terrain_polygon)))
			reshade::log::message(reshade::log::level::info, "mm7fx: hooked DrawTerrainPolygon");
		else
			reshade::log::message(reshade::log::level::warning, "mm7fx: DrawTerrainPolygon prologue mismatch, water detection disabled");
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
