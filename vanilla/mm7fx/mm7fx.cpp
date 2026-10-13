// mm7fx: ReShade add-on for vanilla MM7 (D3D7 -> dxwrapper Dd7to9 -> D3D9 -> ReShade).
// Target: GOG mm7.exe 1.21 + GrayFace patch 2.5.7. Addresses: ../../re/water.md
//
// v2 (water proof of concept):
//   - detour Render::DrawTerrainPolygon: while it runs we know which terrain tile is being drawn;
//   - water tiles (Polygon::flags & 2): at the vanilla draw we draw the tile ourselves with our pixel shader
//     (screen-space reflection from the previous frame's 3D scene + ripples) and skip the vanilla
//     passes, keeping the game's draw order and occlusion;
//   - shore tiles (flags & 0x100, water baked into a sand/grass texture) go through the same batch, but the shader
//     masks the effect per texel by colour (bluish = water), so sand/grass stay the plain base texture;
//   - detour Render::DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene: copy the finished 3D scene
//     (before billboards/UI) as the reflection source for the next frame;
//   - horizon line is derived from the captured vertices (world/view/screen), no FOV guessing.
//   - planar mirror (v3): terrain tiles and building faces (Render::DrawPolygon) are captured in view space,
//     mirrored about the water plane and re-rendered from the game's own projection into an offscreen target;
//     the sky comes from a copy of the frame taken right after the sky pass (angular mirror is exact at infinity).
//   F10 cycles: water effect -> vanilla -> hide water -> hide all terrain.
#include <reshade.hpp>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <windows.h>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "mm7fx";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Might and Magic VII graphics experiments.";

// ABI note: this add-on is built with MinGW GCC, ReShade with MSVC. MSVC returns user-defined types from member
// functions through a hidden pointer, GCC (for 8-byte structs) in EDX:EAX. Never call ReShade methods that return
// structs (get_back_buffer, get_resource_desc, get_resource_from_view, ...) directly. Rendering below goes through
// the native IDirect3DDevice9 (COM, __stdcall) instead.

static void log_info(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	reshade::log::message(reshade::log::level::info, buf);
}

// ---- game side -------------------------------------------------------------------------------

namespace mm7 {
constexpr uintptr_t DrawTerrainPolygon = 0x4A264C;      // __thiscall(Render*, n, Polygon*, tex, transparent, clamp)
constexpr uint8_t DrawTerrainPolygonPrologue[] = {0x55, 0x8B, 0xEC, 0x51, 0x53};
constexpr uintptr_t EndSceneBillboards = 0x4A1F71;      // __thiscall(Render*), draws billboards/effects, EndScene
constexpr uint8_t EndSceneBillboardsPrologue[] = {0xFF, 0x89, 0x0C, 0x01, 0x04, 0x00}; // dec [ecx+0x4010C]
constexpr uintptr_t DrawPolygon = 0x4A1FE0;             // __thiscall(Render*, n, Polygon*, ODMFace*, tex); building faces
constexpr uint8_t DrawPolygonPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10}; // push ebp; mov ebp,esp; sub esp,10h
constexpr uintptr_t DrawBuildingsD3D = 0x478411;        // __thiscall(Render*); terrain is done when it starts
constexpr uint8_t DrawBuildingsD3DPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C}; // push ebp; mov ebp,esp; sub esp,4Ch
constexpr size_t RenderSceneDepth = 0x4010C;            // BeginScene nesting counter
constexpr uintptr_t SoftVertices = 0x50AA90;            // RenderVertexSoft[], stride 0x30
constexpr uintptr_t TLVertices = 0x77E448;              // D3DTLVERTEX[], stride 0x20, filled by DrawTerrainPolygon
constexpr size_t PolygonFlags = 0x30;                   // i16
constexpr uint16_t TileWater = 2;
constexpr uintptr_t RenderRect = 0xF8BAA4;             // RECT: game 3D view in game screen coords (GrayFace _RenderRect)
constexpr uint16_t TileShore = 0x100;                   // shore: water + sand/grass in one texture
// The game always renders in a 640x480 logical screen; dxwrapper stretches it to the D3D9 back buffer.
// ponytail: assumes a plain stretch (no letterbox); read dxwrapper's scaling if a mode ever letterboxes.
constexpr float ScreenW = 640.0f, ScreenH = 480.0f;

struct SoftVertex { float wx, wy, wz, vx, vy, vz, px, py, rhw, u, v, pad; };
struct TLVertex { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; };
static_assert(sizeof(SoftVertex) == 0x30 && sizeof(TLVertex) == 0x20);
}

// 5+ byte JMP detour; the overwritten bytes must be whole, position-independent instructions.
template <size_t N>
static bool install_detour(uintptr_t target, const uint8_t (&expected)[N], void *hook, void **original)
{
	static_assert(N >= 5);
	if (std::memcmp(reinterpret_cast<void *>(target), expected, N) != 0)
		return false; // someone else (e.g. GrayFace) already patched it, or a different exe

	auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, N + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!tramp)
		return false;
	std::memcpy(tramp, expected, N);
	tramp[N] = 0xE9;
	*reinterpret_cast<int32_t *>(tramp + N + 1) = static_cast<int32_t>(target + N - reinterpret_cast<uintptr_t>(tramp + N + 5));
	*original = tramp;

	DWORD old;
	VirtualProtect(reinterpret_cast<void *>(target), N, PAGE_EXECUTE_READWRITE, &old);
	auto *p = reinterpret_cast<uint8_t *>(target);
	p[0] = 0xE9;
	*reinterpret_cast<int32_t *>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (target + 5));
	for (size_t i = 5; i < N; ++i)
		p[i] = 0x90;
	VirtualProtect(reinterpret_cast<void *>(target), N, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(target), N);
	return true;
}

// ---- state -----------------------------------------------------------------------------------

enum class Mode { Effect, Vanilla, HideWater, HideTerrain, DebugSolid, DebugReflection };
static Mode s_mode = Mode::Effect;
static bool s_f10_was_down = false;

static IDirect3DDevice9 *s_dev = nullptr;
static uint8_t *s_current_poly = nullptr; // set only while the game is inside DrawTerrainPolygon
static uint32_t s_current_n = 0;          // vertex count after clipping (DrawTerrainPolygon arg, not Polygon::uNumVertices)
static bool s_poly_captured = false;
static IDirect3DBaseTexture9 *s_poly_tex = nullptr;
static IDirect3DBaseTexture9 *s_poly_overlay = nullptr; // alpha-blended layer of a shore tile (AddRef'd)
static uint32_t s_poly_draws = 0;
static DWORD s_overlay_src = D3DBLEND_SRCALPHA, s_overlay_dst = D3DBLEND_INVSRCALPHA; // base-pass texture of the current tile (AddRef'd)

// Raw game TL vertex (640x480 screen, z, rhw). Our vertex shader converts it to clip space like dxwrapper's
// XYZRHW fix-up shader does, so the GPU clips polygons with vertices behind/near the camera correctly.
struct WaterVertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
static const D3DVERTEXELEMENT9 s_water_decl_elems[] = {
	{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
	{0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
	{0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
	D3DDECL_END()};
// Shore tiles are drawn by vanilla in two layers: water first, then the shore texture alpha-blended on top.
// tex = water layer, overlay = shore layer (null for pure water).
struct WaterPoly { IDirect3DBaseTexture9 *tex; uint32_t first, count; bool shore; IDirect3DBaseTexture9 *overlay; };
static std::vector<WaterVertex> s_water_verts;
static std::vector<WaterPoly> s_water_polys;

// horizon fit: screen y = a + b * (v_vert / v_fwd); camera "up" from world-axis deltas of tile corners
struct Fit { double n = 0, st = 0, sy = 0, stt = 0, sty = 0; float up[3] = {}; bool have_up = false; };
static Fit s_fit;
static D3DVIEWPORT9 s_game_vp = {}; // D3D9 viewport the game's water was drawn with
static IDirect3DSurface9 *s_game_rt = nullptr; // render target the game's water was drawn into (AddRef'd)
static float s_dxw_off[4] = {}, s_dxw_mul[4] = {}; // dxwrapper XYZRHW fix-up shader constants c0/c1 at that draw
static DWORD s_game_zenable = D3DZB_TRUE, s_game_zfunc = D3DCMP_LESSEQUAL, s_game_zwrite = TRUE; // at the vanilla water draw
static float s_zmin = 9, s_zmax = -9, s_rmin = 9, s_rmax = -9;
static float s_horizon_px = -1.0f, s_fit_a = 0, s_fit_b = 0, s_fit_T = 0;

static uint32_t s_frame = 0, s_draws = 0, s_water_draws = 0, s_terrain_draws = 0;
static DWORD s_last_log = 0;

// ---- GrayFace HD scaling -----------------------------------------------------------------------
// GrayFace's DXProxy DrawPrimitive copies the game's TL vertices into its VertexBuf and rescales sx/sy
// (x*Mul + ShiftX or x*RenderW/ScreenW) before passing them on (MMPatches/MMCommon/DXProxy.pas).
// We locate that buffer once by content, then read the effective affine map at every water draw.
static const mm7::TLVertex *const *s_gf_vbuf = nullptr; // address of GrayFace's VertexBuf pointer
static float s_gf_mul[2] = {1, 1}, s_gf_add[2] = {0, 0};

static bool readable(const void *p, size_t n)
{
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return false;
	return static_cast<const uint8_t *>(p) + n <= static_cast<const uint8_t *>(mi.BaseAddress) + mi.RegionSize;
}

static bool same_except_xy(const mm7::TLVertex &a, const mm7::TLVertex &b)
{
	return a.z == b.z && a.rhw == b.rhw && a.u == b.u && a.v == b.v && a.diffuse == b.diffuse;
}

static void find_grayface_vbuf(uint32_t n)
{
	static bool tried = false;
	if (tried || n < 3)
		return;
	tried = true;
	HMODULE h = GetModuleHandleA("MM7patch.dll");
	if (!h) {
		log_info("mm7fx: MM7patch.dll not loaded, assuming no GrayFace scaling");
		return;
	}
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(h);
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(h) + dos->e_lfanew);
	const auto *base = reinterpret_cast<const uint8_t *>(h);
	const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
	for (uint32_t off = 0; off + 4 <= nt->OptionalHeader.SizeOfImage; off += 4) {
		if (!readable(base + off, 4))
			continue;
		const auto *cand = *reinterpret_cast<const mm7::TLVertex *const *>(base + off);
		if (!cand || cand == tl || !readable(cand, n * sizeof(mm7::TLVertex)))
			continue;
		bool ok = true;
		for (uint32_t i = 0; i < n && ok; ++i)
			ok = same_except_xy(cand[i], tl[i]);
		if (ok) {
			s_gf_vbuf = reinterpret_cast<const mm7::TLVertex *const *>(base + off);
			log_info("mm7fx: GrayFace VertexBuf pointer at MM7patch.dll+0x%x", off);
			return;
		}
	}
	log_info("mm7fx: GrayFace VertexBuf not found, assuming no scaling");
}

// Called inside a vanilla water draw: GrayFace's buffer holds the scaled copy of the game's TL vertices.
static void update_grayface_scale(uint32_t n)
{
	find_grayface_vbuf(n);
	if (!s_gf_vbuf || n < 2)
		return;
	const mm7::TLVertex *vb = *s_gf_vbuf;
	const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
	if (!vb || !readable(vb, n * sizeof(mm7::TLVertex)) || !same_except_xy(vb[0], tl[0]))
		return;
	for (int axis = 0; axis < 2; ++axis) {
		auto c = [&](const mm7::TLVertex &v) { return axis ? v.y : v.x; };
		for (uint32_t j = 1; j < n; ++j) {
			const float d = c(tl[j]) - c(tl[0]);
			if (std::fabs(d) > 1.0f) {
				s_gf_mul[axis] = (c(vb[j]) - c(vb[0])) / d;
				s_gf_add[axis] = c(vb[0]) - c(tl[0]) * s_gf_mul[axis];
				break;
			}
		}
	}
}

// ---- capture ---------------------------------------------------------------------------------

static void capture_water_poly()
{
	const uint32_t n = s_current_n;
	if (n < 3 || n > 64 || !s_dev)
		return;
	const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
	const auto *sv = reinterpret_cast<const mm7::SoftVertex *>(mm7::SoftVertices);


	if (s_poly_tex) { // depth setup of the water layer, not of an alpha-blended shore layer
		s_dev->GetRenderState(D3DRS_ZENABLE, &s_game_zenable);
		s_dev->GetRenderState(D3DRS_ZFUNC, &s_game_zfunc);
		s_dev->GetRenderState(D3DRS_ZWRITEENABLE, &s_game_zwrite);
	}
	const bool shore = !(*reinterpret_cast<const int16_t *>(s_current_poly + mm7::PolygonFlags) & mm7::TileWater);
	// second call of a shore tile (alpha-blended shore layer): kept as an overlay-only poly, drawn after all water;
	// the shore tile's water layer (first call) is then plain water
	const bool layer = s_poly_overlay && !s_poly_tex;
	WaterPoly wp = {s_poly_tex, static_cast<uint32_t>(s_water_verts.size()), n, shore && !layer && false, s_poly_overlay};
	s_poly_tex = nullptr; // references move to the poly
	s_poly_overlay = nullptr;
	// consistency check: TL x/y must equal the soft vertices' projected x/y
	static uint32_t mism = 0, total = 0;
	for (uint32_t i = 0; i < n; ++i) {
		++total;
		if (std::fabs(tl[i].x - sv[i].px) > 0.5f || std::fabs(tl[i].y - sv[i].py) > 0.5f) ++mism;
	}
	static DWORD last_chk = 0;
	if (GetTickCount() - last_chk > 5000) {
		last_chk = GetTickCount();
		log_info("mm7fx dbg: TL vs soft projection mismatches %u / %u", mism, total);
		mism = total = 0;
	}
	for (uint32_t i = 0; i < n; ++i) {
		const auto &t = tl[i];
		s_zmin = t.z < s_zmin ? t.z : s_zmin; s_zmax = t.z > s_zmax ? t.z : s_zmax;
		s_rmin = t.rhw < s_rmin ? t.rhw : s_rmin; s_rmax = t.rhw > s_rmax ? t.rhw : s_rmax;
		// -0.5: D3D9 rasterizes pixel centers at integer coords, D3D7 TL vertices at +0.5
		s_water_verts.push_back({t.x, t.y, t.z, t.rhw, t.diffuse, t.u, t.v});
	}
	s_water_polys.push_back(wp);
	static DWORD last = 0;
	if (GetTickCount() - last > 3000 && (n > 4 || !wp.tex || (tl[0].diffuse & 0xFFFFFF) < 0x202020)) {
		last = GetTickCount();
		DWORD op = 0, a1 = 0, a2 = 0;
		s_dev->GetTextureStageState(0, D3DTSS_COLOROP, &op);
		s_dev->GetTextureStageState(0, D3DTSS_COLORARG1, &a1);
		s_dev->GetTextureStageState(0, D3DTSS_COLORARG2, &a2);
		log_info("mm7fx dbg: odd water poly n=%u tex=%p diffuse0=%08x spec0=%08x uv0=(%.2f,%.2f) rhw0=%.5f z0=%.5f colorop=%lu args=%lu,%lu",
			n, (void *)wp.tex, tl[0].diffuse, tl[0].specular, tl[0].u, tl[0].v, tl[0].rhw, tl[0].z, op, a1, a2);
	}

	// projection fit samples (view: x = forward depth, z = vertical)
	for (uint32_t i = 0; i < n; ++i) {
		if (sv[i].vx <= 1.0f)
			continue;
		const double tz = sv[i].vz / sv[i].vx;
		s_fit.n += 1; s_fit.st += tz; s_fit.sy += tl[i].y; s_fit.stt += tz * tz; s_fit.sty += tz * tl[i].y;
	}
	// camera up vector: water is flat, tile corners differ by +-512 in world x or y
	if (!s_fit.have_up) {
		float rx[3] = {}, ry[3] = {};
		bool gx = false, gy = false;
		for (uint32_t i = 0; i < n && !(gx && gy); ++i)
			for (uint32_t j = 0; j < n; ++j) {
				const float dwx = sv[j].wx - sv[i].wx, dwy = sv[j].wy - sv[i].wy;
				if (std::fabs(sv[j].wz - sv[i].wz) > 1) // shore tiles are not flat
					continue;
				const float dv[3] = {sv[j].vx - sv[i].vx, sv[j].vy - sv[i].vy, sv[j].vz - sv[i].vz};
				// near-clipped vertices keep stale world positions: the view delta must be as long as the world delta
				const float dvl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
				if (!gx && std::fabs(dwx) > 100 && std::fabs(dwy) < 1 && std::fabs(dvl - std::fabs(dwx)) < 0.01f * std::fabs(dwx)) { for (int k = 0; k < 3; ++k) rx[k] = dv[k] / dwx; gx = true; }
				if (!gy && std::fabs(dwy) > 100 && std::fabs(dwx) < 1 && std::fabs(dvl - std::fabs(dwy)) < 0.01f * std::fabs(dwy)) { for (int k = 0; k < 3; ++k) ry[k] = dv[k] / dwy; gy = true; }
			}
		if (gx && gy) {
			s_fit.up[0] = rx[1] * ry[2] - rx[2] * ry[1];
			s_fit.up[1] = rx[2] * ry[0] - rx[0] * ry[2];
			s_fit.up[2] = rx[0] * ry[1] - rx[1] * ry[0];
			s_fit.have_up = true;
		}
	}
}

static void solve_horizon()
{
	s_horizon_px = -1.0f;
	const Fit &f = s_fit;
	const double det = f.n * f.stt - f.st * f.st;
	if (f.n < 3 || std::fabs(det) < 1e-9 || !f.have_up || std::fabs(f.up[2]) < 1e-6f)
		return;
	const double b = (f.n * f.sty - f.st * f.sy) / det;
	const double a = (f.sy - b * f.st) / f.n;
	// horizon: view directions (1, 0, t) perpendicular to up -> t = -up.x / up.z
	const double th = -f.up[0] / f.up[2];
	s_horizon_px = static_cast<float>(a + b * th);
	s_fit_a = static_cast<float>(a);
	s_fit_b = static_cast<float>(b);
	s_fit_T = static_cast<float>(2 * th / (1 - th * th)); // tan(2 * atan(th))
}

// ---- water pass ------------------------------------------------------------------------------

static const char s_water_vs_src[] = R"(
struct VSIn { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
float4 offset : register(c0);     // dxwrapper's fix-up constants, captured at the vanilla water draw
float4 multiplier : register(c1);
float4 gf : register(c2);         // GrayFace HD scaling: xy * gf.xy + gf.zw
VSOut main(VSIn i)
{
	// identical to dxwrapper's XYZRHW fix-up shader
	float rhw = clamp(i.pos.w, 1.0 / 2147483648.0, 2147483648.0);
	float w = 1.0 / rhw;
	VSOut o;
	float4 p = i.pos;
	p.xy = p.xy * gf.xy + gf.zw;
	o.pos = (p + offset) * multiplier;
	o.pos.z = saturate(o.pos.z);
	o.pos.xyz *= w;
	o.pos.w = w;
	o.color = i.color;
	o.uv = i.uv;
	return o;
}
)";

// shore layer drawn over our water: plain texture * vertex colour, alpha from the texture (like vanilla)
static const char s_overlay_ps_src[] = R"(
sampler2D layerTex : register(s1);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
	float4 t = tex2D(layerTex, uv);
	// transparent texels are black: bilinear filtering darkens the edge colour (dark outline along the shore);
	// undo it by dividing by the filtered alpha
	float3 c = t.rgb / max(t.a, 1e-3);
	return float4(saturate(c) * color.rgb, t.a); // vertex alpha is not meaningful for terrain
}
)";

static const char s_water_ps_src[] = R"(
sampler2D sceneTex : register(s0);
sampler2D waterTex : register(s1);
float4 params : register(c0); // x = time (s), y = fit a (px), z = fit b (px), w = T = tan(2 * horizon angle)
float4 extra : register(c1);  // x = reflection strength, y = horizon (game px), z = 1/480, w = debug solid
float4 rt : register(c2);     // x = 1/RT width, y = 1/RT height
float4 vp : register(c3);     // viewport X, Y, W, H
float4 off : register(c4);    // dxwrapper fix-up offset
float4 mul : register(c5);    // dxwrapper fix-up multiplier
float4 gf : register(c6);     // GrayFace HD scaling: xy * gf.xy + gf.zw
float4 mir : register(c7);    // x = planar mirror in mirrorTex (sceneTex = sky only), y = debug: reflection only
float4 shore : register(c8);  // x = 1: single-layer shore tile, effect masked to water-coloured texels
float4 view : register(c9);   // game 3D view rect (game screen coords): left, top, right, bottom
sampler2D mirrorTex : register(s2);
// "looks like water": blue clearly above red (sand/grass/dirt have b < r) and not grey (rock, foam)
static const float2 BlueMinusRed = float2(0.04, 0.12); // smoothstep edges of b - r
static const float2 Saturation = float2(0.10, 0.22);   // smoothstep edges of (max - min) / max
float water_mask(float3 t)
{
	float mx = max(t.r, max(t.g, t.b));
	float sat = (mx - min(t.r, min(t.g, t.b))) / max(mx, 1e-3);
	return smoothstep(BlueMinusRed.x, BlueMinusRed.y, t.b - t.r) * smoothstep(Saturation.x, Saturation.y, sat);
}
float2 to_game(float2 px)     // RT pixel -> game TL screen coords
{
	float2 ndc = float2((px.x - vp.x) / vp.z * 2.0 - 1.0, 1.0 - (px.y - vp.y) / vp.w * 2.0);
	return ((ndc / mul.xy - off.xy) - gf.zw) / gf.xy;
}
float2 to_rt_uv(float2 g)     // game TL screen coords -> RT uv
{
	float2 ndc = (g * gf.xy + gf.zw + off.xy) * mul.xy;
	return float2(vp.x + (ndc.x + 1.0) * 0.5 * vp.z, vp.y + (1.0 - ndc.y) * 0.5 * vp.w) * rt.xy;
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
	float2 gp = to_game(vpos + 0.5);
	float2 suv = gp * extra.z;                         // ~0..1 scale for ripple frequencies
	float time = params.x;
	float2 rip = float2(sin(suv.y * 260.0 + time * 2.1) + sin(suv.x * 90.0 - time * 1.3),
	                    sin(suv.x * 170.0 + time * 1.7)) * 0.0025;
	float y = gp.y;                                    // game screen px
	float t = (y - params.y) / params.z;               // tan of the view-space vertical angle
	float tr = (params.w - t) / (1.0 + params.w * t);  // mirrored about the horizon angle
	float yr = params.y + params.z * tr;               // reflected point, game px
	float below = max(y - extra.y, 0.0) * extra.z;
	float2 ruv = to_rt_uv(float2(gp.x, yr)) + rip * (1.0 + below * 6.0);
	// valid image = the game's 3D view; the render target around it is not drawn (black)
	float2 vlo = to_rt_uv(view.xy), vhi = to_rt_uv(view.zw);
	float2 lo = min(vlo, vhi), hi = max(vlo, vhi) - rt.xy;
	float top = lo.y;
	float inside = saturate((ruv.y - top) * 8.0);      // reflected ray left the top of the view: no data
	ruv = clamp(ruv, lo, hi);
	float3 refl = tex2D(sceneTex, ruv).rgb;
	// planar mirror: mirrored geometry rendered for this very pixel (alpha = coverage), the sky behind it is
	// exact under the angular mirror, so clamping at the top only stretches the zenith sky
	float4 mr = tex2D(mirrorTex, clamp((vpos + 0.5) * rt.xy + rip * (1.0 + below * 6.0), lo, hi));
	refl = lerp(refl, mr.rgb, mr.a * mir.x);
	inside = lerp(inside, 1.0, mir.x);
	if (mir.y > 0.5)
		return float4(refl, 1.0);
	float m = shore.x > 0.5 ? water_mask(tex2D(waterTex, uv).rgb) : 1.0;
	float3 base = tex2D(waterTex, uv + rip * 4.0 * m).rgb * color.rgb; // m = 0: exactly the base texture
	float fresnel = saturate(1.0 - below * 3.5);
	float k = extra.x * (0.35 + 0.55 * fresnel) * inside * m;
	float3 c = lerp(base, refl * float3(0.85, 0.92, 1.0), k);
	float3 dbg = shore.x > 0.5 ? float3(0.0, 1.0, 1.0) : float3(1.0, 0.0, 1.0);
	return float4(lerp(c, dbg, extra.w), 1.0); // extra.w = 1: debug solid magenta (water) / cyan (shore)
}
)";

static IDirect3DPixelShader9 *s_water_ps = nullptr;
static IDirect3DPixelShader9 *s_overlay_ps = nullptr;
static IDirect3DVertexShader9 *s_water_vs = nullptr;
static IDirect3DVertexDeclaration9 *s_water_decl = nullptr;
static IDirect3DTexture9 *s_scene_tex = nullptr;
static UINT s_scene_w = 0, s_scene_h = 0;
static IDirect3DStateBlock9 *s_state = nullptr;
static bool s_init_failed = false;

static bool init_water_resources()
{
	if (s_water_ps || s_init_failed)
		return s_water_ps != nullptr;
	s_init_failed = true;

	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	auto compile = dc ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dc, "D3DCompile")) : nullptr;
	if (!compile) {
		log_info("mm7fx: d3dcompiler_47.dll / D3DCompile not available");
		return false;
	}
	auto build = [&](const char *src, size_t len, const char *target, ID3DBlob **out) {
		ID3DBlob *err = nullptr;
		const HRESULT hr = compile(src, len, "mm7fx_water", nullptr, nullptr, "main", target, 0, 0, out, &err);
		if (FAILED(hr))
			log_info("mm7fx: %s compile failed 0x%08lx: %s", target, hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
		if (err) err->Release();
		return SUCCEEDED(hr);
	};
	ID3DBlob *vs = nullptr, *ps = nullptr, *ops = nullptr;
	if (!build(s_water_vs_src, sizeof(s_water_vs_src) - 1, "vs_3_0", &vs) || !build(s_water_ps_src, sizeof(s_water_ps_src) - 1, "ps_3_0", &ps)
		|| !build(s_overlay_ps_src, sizeof(s_overlay_ps_src) - 1, "ps_3_0", &ops)) {
		if (vs) vs->Release();
		if (ps) ps->Release();
		return false;
	}
	HRESULT hr = s_dev->CreateVertexShader(static_cast<const DWORD *>(vs->GetBufferPointer()), &s_water_vs);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(static_cast<const DWORD *>(ps->GetBufferPointer()), &s_water_ps);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(static_cast<const DWORD *>(ops->GetBufferPointer()), &s_overlay_ps);
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexDeclaration(s_water_decl_elems, &s_water_decl);
	if (SUCCEEDED(hr)) hr = s_dev->CreateStateBlock(D3DSBT_ALL, &s_state);
	vs->Release();
	ps->Release();
	ops->Release();
	if (FAILED(hr)) {
		log_info("mm7fx: creating water shaders/decl/state block failed 0x%08lx", hr);
		return false;
	}
	s_init_failed = false;
	log_info("mm7fx: water shader ready");
	return true;
}

static void release_captured()
{
	for (auto &p : s_water_polys) {
		if (p.tex) p.tex->Release();
		if (p.overlay) p.overlay->Release();
	}
	s_water_polys.clear();
	s_water_verts.clear();
	if (s_game_rt) {
		s_game_rt->Release();
		s_game_rt = nullptr;
	}
	s_fit = Fit();
}

static bool s_water_drawn_this_frame = false;
static bool s_scene_ready = false; // s_scene_tex holds the previous frame (3D scene before billboards/UI)

static bool copy_scene()
{
	IDirect3DSurface9 *rt = s_game_rt;
	if (!rt)
		return false;
	rt->AddRef();
	D3DSURFACE_DESC rd;
	rt->GetDesc(&rd);
	if (!s_scene_tex || s_scene_w != rd.Width || s_scene_h != rd.Height) {
		if (s_scene_tex) s_scene_tex->Release();
		s_scene_tex = nullptr;
		if (FAILED(s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, &s_scene_tex, nullptr))) {
			log_info("mm7fx: scene copy texture %ux%u fmt %d failed", rd.Width, rd.Height, rd.Format);
			rt->Release();
			return false;
		}
		s_scene_w = rd.Width;
		s_scene_h = rd.Height;
		log_info("mm7fx: scene copy texture %ux%u fmt %d", rd.Width, rd.Height, rd.Format);
	}
	IDirect3DSurface9 *dst = nullptr;
	s_scene_tex->GetSurfaceLevel(0, &dst);
	const HRESULT hr = s_dev->StretchRect(rt, nullptr, dst, nullptr, D3DTEXF_NONE);
	dst->Release();
	rt->Release();
	if (FAILED(hr))
		log_info("mm7fx: StretchRect failed 0x%08lx", hr);
	return SUCCEEDED(hr);
}

// ---- planar mirror pass ----------------------------------------------------------------------
// Render::DrawTerrainPolygon and Render::DrawPolygon (building faces) both draw from the soft vertex array at
// 0x50AA90 after clipping, so view-space positions are valid for every vertex (near/far-clipped ones have stale
// world positions, which is why everything here stays in view space: x = forward, y = left, z = up).
// The game projects px = cx - f * y / x, py = cy - f * z / x; we fit both lines per frame from the captured
// vertices, mirror view positions about the water plane (normal = camera "up" from the horizon fit) and push them
// through the same GrayFace + dxwrapper mapping as the water, as homogeneous clip coordinates so the GPU clips.

struct MirrorVertex { float x, y, z; uint32_t diffuse; float u, v; };
static const D3DVERTEXELEMENT9 s_mirror_decl_elems[] = {
	{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
	{0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
	{0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
	D3DDECL_END()};
struct MirrorPoly { IDirect3DBaseTexture9 *tex; uint32_t first, count; };

struct LinFit {
	double n = 0, st = 0, sy = 0, stt = 0, sty = 0;
	void add(double t, double y) { n += 1; st += t; sy += y; stt += t * t; sty += t * y; }
	bool solve(float &a, float &b) const
	{
		const double det = n * stt - st * st;
		if (n < 8 || std::fabs(det) < 1e-9)
			return false;
		b = static_cast<float>((n * sty - st * sy) / det);
		a = static_cast<float>((sy - b * st) / n);
		return true;
	}
};

static std::vector<MirrorVertex> s_mir_verts, s_mir_tris;
static std::vector<MirrorPoly> s_mir_polys;
static std::vector<uint32_t> s_mir_order;
static uint32_t s_mir_n = 0;                       // > 0 while inside a hooked polygon draw
static bool s_mir_have_verts = false;              // vertices of the current polygon taken at its first draw
static MirrorVertex s_mir_cur[64];
static IDirect3DBaseTexture9 *s_mir_cur_tex = nullptr; // first non-null texture of the current polygon (AddRef'd)
static LinFit s_mir_fit_x, s_mir_fit_y;
static double s_mir_water_sum[3] = {};             // view-space sum of water vertices -> plane offset
static uint32_t s_mir_water_cnt = 0;
static bool s_sky_pending = false, s_sky_ready = false;
static IDirect3DTexture9 *s_sky_tex = nullptr;     // frame right after the sky pass (sky only)
static IDirect3DTexture9 *s_mirror_tex = nullptr;  // mirrored geometry, alpha = coverage
static IDirect3DSurface9 *s_mirror_depth = nullptr;
static IDirect3DVertexShader9 *s_mirror_vs = nullptr;
static IDirect3DPixelShader9 *s_mirror_ps = nullptr;
static IDirect3DVertexDeclaration9 *s_mirror_decl = nullptr;
static bool s_mirror_init_failed = false;

static bool mirror_wanted()
{
	return s_mode == Mode::Effect || s_mode == Mode::DebugSolid || s_mode == Mode::DebugReflection;
}

static void mirror_release_captured()
{
	for (auto &p : s_mir_polys)
		if (p.tex) p.tex->Release();
	s_mir_polys.clear();
	s_mir_verts.clear();
	s_mir_fit_x = LinFit();
	s_mir_fit_y = LinFit();
	s_mir_water_sum[0] = s_mir_water_sum[1] = s_mir_water_sum[2] = 0;
	s_mir_water_cnt = 0;
	s_sky_ready = false;
}

// Copies a render target into a same-sized texture (created on demand).
static bool copy_rt_to_tex(IDirect3DSurface9 *rt, IDirect3DTexture9 *&tex)
{
	D3DSURFACE_DESC rd, td;
	rt->GetDesc(&rd);
	if (tex && (tex->GetLevelDesc(0, &td), td.Width != rd.Width || td.Height != rd.Height || td.Format != rd.Format)) {
		tex->Release();
		tex = nullptr;
	}
	if (!tex && FAILED(s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, &tex, nullptr)))
		return false;
	IDirect3DSurface9 *dst = nullptr;
	tex->GetSurfaceLevel(0, &dst);
	const HRESULT hr = s_dev->StretchRect(rt, nullptr, dst, nullptr, D3DTEXF_NONE);
	dst->Release();
	return SUCCEEDED(hr);
}

// Called from on_draw for every draw. Sky copy at the first terrain/building draw of the frame (the sky pass is done
// by then), vertices + base texture of the current polygon at its first draw (later passes overwrite diffuse).
static void mirror_on_draw()
{
	if (!s_mir_n || !s_dev)
		return;
	if (s_sky_pending) {
		s_sky_pending = false;
		IDirect3DSurface9 *rt = nullptr;
		if (SUCCEEDED(s_dev->GetRenderTarget(0, &rt)) && rt) {
			s_sky_ready = copy_rt_to_tex(rt, s_sky_tex);
			rt->Release();
		}
	}
	if (!s_mir_have_verts) {
		s_mir_have_verts = true;
		const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
		const auto *sv = reinterpret_cast<const mm7::SoftVertex *>(mm7::SoftVertices);
		for (uint32_t i = 0; i < s_mir_n; ++i)
			s_mir_cur[i] = {sv[i].vx, sv[i].vy, sv[i].vz, tl[i].diffuse, tl[i].u, tl[i].v};
	}
	if (!s_mir_cur_tex)
		s_dev->GetTexture(0, &s_mir_cur_tex);
}

static void mirror_begin(uint32_t n)
{
	s_mir_n = mirror_wanted() && n >= 3 && n <= 64 ? n : 0;
	s_mir_have_verts = false;
	s_mir_cur_tex = nullptr;
}

static void mirror_end(const uint8_t *poly)
{
	const uint32_t n = s_mir_n;
	s_mir_n = 0;
	if (!n)
		return;
	const auto *sv = reinterpret_cast<const mm7::SoftVertex *>(mm7::SoftVertices);
	for (uint32_t i = 0; i < n; ++i)
		if (sv[i].vx > 1.0f) {
			s_mir_fit_x.add(sv[i].vy / sv[i].vx, sv[i].px);
			s_mir_fit_y.add(sv[i].vz / sv[i].vx, sv[i].py);
		}
	const int16_t flags = poly ? *reinterpret_cast<const int16_t *>(poly + mm7::PolygonFlags) : 0;
	const bool water = flags & mm7::TileWater;
	// shore tiles (water layer + shore layer at water level) must not be mirrored either: their flat water layer
	// would cover the mirrored landscape near the coast
	if (!water && (flags & mm7::TileShore)) {
	} else if (water) {
		for (uint32_t i = 0; i < n; ++i) {
			s_mir_water_sum[0] += sv[i].vx; s_mir_water_sum[1] += sv[i].vy; s_mir_water_sum[2] += sv[i].vz;
		}
		s_mir_water_cnt += n;
	} else if (s_mir_have_verts && s_mir_cur_tex) {
		s_mir_polys.push_back({s_mir_cur_tex, static_cast<uint32_t>(s_mir_verts.size()), n});
		s_mir_cur_tex = nullptr; // reference moves to the poly
		s_mir_verts.insert(s_mir_verts.end(), s_mir_cur, s_mir_cur + n);
	}
	if (s_mir_cur_tex) {
		s_mir_cur_tex->Release();
		s_mir_cur_tex = nullptr;
	}
}

static const char s_mirror_vs_src[] = R"(
struct VSIn { float3 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float dist : TEXCOORD1; };
float4 plane : register(c0); // view-space water plane dot(n, p) = d, n up
float4 proj : register(c1);  // game px = proj.x + proj.y * y / x, py = proj.z + proj.w * z / x
float4 gf : register(c2);    // GrayFace HD scaling
float4 off : register(c3);   // dxwrapper fix-up offset
float4 mul : register(c4);   // dxwrapper fix-up multiplier
float4 zr : register(c5);    // x = near, y = far / (far - near)
VSOut main(VSIn i)
{
	VSOut o;
	o.dist = dot(plane.xyz, i.pos) - plane.w;
	float3 p = i.pos - 2.0 * o.dist * plane.xyz;
	float w = p.x;
	float2 gw = float2(proj.x * w + proj.y * p.y, proj.z * w + proj.w * p.z); // game px * w
	o.pos = float4((gw * gf.xy + (gf.zw + off.xy) * w) * mul.xy, (w - zr.x) * zr.y, w);
	o.color = i.color;
	o.uv = i.uv;
	return o;
}
)";

static const char s_mirror_ps_src[] = R"(
sampler2D tex : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float dist : TEXCOORD1) : COLOR
{
	clip(dist + 0.5);           // only geometry above the water plane is reflected
	float4 t = tex2D(tex, uv);
	clip(t.a - 0.5);            // color-keyed texels
	return float4(t.rgb * color.rgb, 1.0);
}
)";

static bool init_mirror_resources()
{
	if (s_mirror_ps || s_mirror_init_failed)
		return s_mirror_ps != nullptr;
	s_mirror_init_failed = true;
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	auto compile = dc ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dc, "D3DCompile")) : nullptr;
	if (!compile)
		return false;
	ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
	HRESULT hr = compile(s_mirror_vs_src, sizeof(s_mirror_vs_src) - 1, "mm7fx_mirror", nullptr, nullptr, "main", "vs_3_0", 0, 0, &vs, &err);
	if (SUCCEEDED(hr)) {
		if (err) { err->Release(); err = nullptr; }
		hr = compile(s_mirror_ps_src, sizeof(s_mirror_ps_src) - 1, "mm7fx_mirror", nullptr, nullptr, "main", "ps_3_0", 0, 0, &ps, &err);
	}
	if (FAILED(hr))
		log_info("mm7fx: mirror shader compile failed 0x%08lx: %s", hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
	if (err) err->Release();
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexShader(static_cast<const DWORD *>(vs->GetBufferPointer()), &s_mirror_vs);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(static_cast<const DWORD *>(ps->GetBufferPointer()), &s_mirror_ps);
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexDeclaration(s_mirror_decl_elems, &s_mirror_decl);
	if (vs) vs->Release();
	if (ps) ps->Release();
	if (FAILED(hr)) {
		log_info("mm7fx: mirror pass init failed 0x%08lx, screen-space reflection only", hr);
		return false;
	}
	s_mirror_init_failed = false;
	log_info("mm7fx: mirror pass ready");
	return true;
}

static void release_mirror_resources()
{
	mirror_release_captured();
	for (IUnknown **p : {reinterpret_cast<IUnknown **>(&s_sky_tex), reinterpret_cast<IUnknown **>(&s_mirror_tex),
	                     reinterpret_cast<IUnknown **>(&s_mirror_depth), reinterpret_cast<IUnknown **>(&s_mirror_vs),
	                     reinterpret_cast<IUnknown **>(&s_mirror_ps), reinterpret_cast<IUnknown **>(&s_mirror_decl)}) {
		if (*p) (*p)->Release();
		*p = nullptr;
	}
	s_sky_pending = false;
}

// Renders the mirrored scene into s_mirror_tex (same size/viewport as the game's target). Call with s_state captured;
// restores it. False -> use the screen-space fallback.
static bool mirror_render()
{
	if (!s_sky_ready || s_mir_polys.empty() || !s_fit.have_up || !s_mir_water_cnt || !s_game_rt || !init_mirror_resources())
		return false;
	float ax, bx, ay, by;
	if (!s_mir_fit_x.solve(ax, bx) || !s_mir_fit_y.solve(ay, by) || !(bx * by > 0) || bx / by < 0.5f || bx / by > 2.0f)
		return false; // not the expected pinhole projection (cx - f*y/x, cy - f*z/x)
	float nrm[3] = {s_fit.up[0], s_fit.up[1], s_fit.up[2]};
	const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
	if (len < 1e-6f)
		return false;
	float d = 0;
	for (int k = 0; k < 3; ++k) {
		nrm[k] /= len;
		d += nrm[k] * static_cast<float>(s_mir_water_sum[k] / s_mir_water_cnt);
	}
	if (d > 0) { // the camera (view origin) is above the water: dot(n, 0) - d > 0
		for (float &c : nrm) c = -c;
		d = -d;
	}

	D3DSURFACE_DESC rd;
	s_game_rt->GetDesc(&rd);
	D3DSURFACE_DESC md = {};
	if (s_mirror_tex)
		s_mirror_tex->GetLevelDesc(0, &md);
	if (!s_mirror_tex || md.Width != rd.Width || md.Height != rd.Height) {
		if (s_mirror_tex) s_mirror_tex->Release();
		if (s_mirror_depth) s_mirror_depth->Release();
		s_mirror_tex = nullptr;
		s_mirror_depth = nullptr;
		if (FAILED(s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_mirror_tex, nullptr)) ||
		    FAILED(s_dev->CreateDepthStencilSurface(rd.Width, rd.Height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &s_mirror_depth, nullptr))) {
			log_info("mm7fx: mirror target %ux%u failed", rd.Width, rd.Height);
			if (s_mirror_tex) s_mirror_tex->Release();
			s_mirror_tex = nullptr;
			s_mirror_init_failed = true; // don't retry every frame
			return false;
		}
		log_info("mm7fx: mirror target %ux%u", rd.Width, rd.Height);
	}

	// one triangle list per texture
	s_mir_order.resize(s_mir_polys.size());
	for (uint32_t i = 0; i < s_mir_order.size(); ++i)
		s_mir_order[i] = i;
	std::sort(s_mir_order.begin(), s_mir_order.end(), [](uint32_t a, uint32_t b) { return s_mir_polys[a].tex < s_mir_polys[b].tex; });
	s_mir_tris.clear();

	IDirect3DSurface9 *mrt = nullptr, *prev_ds = nullptr;
	s_mirror_tex->GetSurfaceLevel(0, &mrt);
	s_dev->GetDepthStencilSurface(&prev_ds);
	s_dev->SetRenderTarget(0, mrt);
	s_dev->SetDepthStencilSurface(s_mirror_depth);
	s_dev->SetViewport(&s_game_vp);
	s_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
	const float zn = 16.0f, zf = 65536.0f;
	const float vc[24] = {nrm[0], nrm[1], nrm[2], d, ax, bx, ay, by,
	                      s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1],
	                      s_dxw_off[0], s_dxw_off[1], s_dxw_off[2], s_dxw_off[3],
	                      s_dxw_mul[0], s_dxw_mul[1], s_dxw_mul[2], s_dxw_mul[3], zn, zf / (zf - zn), 0, 0};
	s_dev->SetVertexShaderConstantF(0, vc, 6);
	s_dev->SetVertexShader(s_mirror_vs);
	s_dev->SetPixelShader(s_mirror_ps);
	s_dev->SetVertexDeclaration(s_mirror_decl);
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	auto flush = [&](IDirect3DBaseTexture9 *tex) {
		if (s_mir_tris.empty())
			return;
		s_dev->SetTexture(0, tex);
		constexpr size_t chunk = 3 * 20000;
		for (size_t i = 0; i < s_mir_tris.size(); i += chunk) {
			const size_t cnt = std::min(chunk, s_mir_tris.size() - i);
			s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(cnt / 3), &s_mir_tris[i], sizeof(MirrorVertex));
		}
		s_mir_tris.clear();
	};
	IDirect3DBaseTexture9 *cur = nullptr;
	for (uint32_t idx : s_mir_order) {
		const MirrorPoly &p = s_mir_polys[idx];
		if (p.tex != cur) {
			flush(cur);
			cur = p.tex;
		}
		const MirrorVertex *v = &s_mir_verts[p.first];
		for (uint32_t k = 2; k < p.count; ++k) { // fan -> list
			s_mir_tris.push_back(v[0]);
			s_mir_tris.push_back(v[k - 1]);
			s_mir_tris.push_back(v[k]);
		}
	}
	flush(cur);

	s_dev->SetDepthStencilSurface(prev_ds);
	if (prev_ds) prev_ds->Release();
	mrt->Release();
	s_state->Apply();
	static DWORD last = 0;
	if (GetTickCount() - last > 5000) {
		last = GetTickCount();
		log_info("mm7fx dbg: mirror %zu polys, proj x %.1f%+.1f*y/x y %.1f%+.1f*z/x, plane n (%.3f %.3f %.3f) d %.1f",
			s_mir_polys.size(), ax, bx, ay, by, nrm[0], nrm[1], nrm[2], d);
	}
	return true;
}

// Draws all captured water in one batch. Called right before the game draws buildings: all terrain is
// already in the depth buffer, buildings/ships come after (as in vanilla, where water precedes them).
static void draw_water_batch()
{
	if (s_water_polys.empty() || !s_scene_ready || !init_water_resources())
		return;
	solve_horizon();
	if (s_horizon_px < 0)
		return;
	const float params[8] = {GetTickCount() * 0.001f, s_fit_a, s_fit_b, s_fit_T,
	                         0.75f, s_horizon_px, s_dxw_mul[1] != 0 ? std::fabs(s_dxw_mul[1]) * 0.5f : 1.0f / mm7::ScreenH,
	                         s_mode == Mode::DebugSolid ? 1.0f : 0.0f};

	s_state->Capture();
	// The game draws into a sub-rect of a larger render target (window-sized area of a desktop-sized RT), and the
	// viewport left at the end of the scene differs; reuse the viewport terrain was drawn with.
	if (!s_game_rt || !s_game_vp.Width || !s_game_vp.Height)
		return;
	D3DSURFACE_DESC rd = {};
	s_game_rt->GetDesc(&rd);
	// viewport as set during the vanilla water draw (dxwrapper sets it per draw and restores it afterwards)
	D3DVIEWPORT9 vp = s_game_vp;
	const float rtc[16] = {1.0f / rd.Width, 1.0f / rd.Height, 0, 0,
	                       float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height),
	                       s_dxw_off[0], s_dxw_off[1], s_dxw_off[2], s_dxw_off[3],
	                       s_dxw_mul[0], s_dxw_mul[1], s_dxw_mul[2], s_dxw_mul[3]};
	IDirect3DSurface9 *prev_rt = nullptr;
	s_dev->GetRenderTarget(0, &prev_rt);
	const bool planar = mirror_render();
	const float mirc[4] = {planar ? 1.0f : 0.0f, s_mode == Mode::DebugReflection ? 1.0f : 0.0f, 0, 0};
	s_dev->SetRenderTarget(0, s_game_rt);
	s_dev->SetViewport(&vp);
	s_dev->SetVertexShader(s_water_vs);
	s_dev->SetVertexDeclaration(s_water_decl);
	s_dev->SetPixelShader(s_water_ps);
	s_dev->SetPixelShaderConstantF(0, params, 2);
	s_dev->SetPixelShaderConstantF(2, rtc, 4);
	const int32_t *rr = reinterpret_cast<const int32_t *>(mm7::RenderRect);
	const float viewc[4] = {float(rr[0]), float(rr[1]), float(rr[2]), float(rr[3])};
	s_dev->SetPixelShaderConstantF(9, viewc, 1);
	s_dev->SetVertexShaderConstantF(0, s_dxw_off, 1);
	s_dev->SetVertexShaderConstantF(1, s_dxw_mul, 1);
	const float gfc[4] = {s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1]};
	s_dev->SetVertexShaderConstantF(2, gfc, 1);
	s_dev->SetPixelShaderConstantF(6, gfc, 1);
	s_dev->SetPixelShaderConstantF(7, mirc, 1);
	// same depth setup the game used for its water (it may be a W-buffer)
	s_dev->SetRenderState(D3DRS_ZENABLE, s_mode == Mode::DebugSolid ? D3DZB_FALSE : s_game_zenable); // debug: no depth test
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, s_game_zwrite);
	s_dev->SetRenderState(D3DRS_ZFUNC, s_game_zfunc);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	s_dev->SetTexture(0, planar ? s_sky_tex : s_scene_tex);
	s_dev->SetTexture(2, planar ? s_mirror_tex : nullptr);
	s_dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DBaseTexture9 *bound = nullptr;
	int shore = -1;
	for (const auto &p : s_water_polys) {
		if (!p.tex)
			continue; // overlay-only (shore layer)
		if (p.tex != bound) {
			s_dev->SetTexture(1, p.tex);
			bound = p.tex;
		}
		if (p.shore != shore) {
			shore = p.shore;
			const float sc[4] = {float(shore), 0, 0, 0};
			s_dev->SetPixelShaderConstantF(8, sc, 1);
		}
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_water_verts[p.first], sizeof(WaterVertex));
	}
	// shore layers on top of the water, blended like vanilla did
	if (s_mode == Mode::Effect) {
		s_dev->SetPixelShader(s_overlay_ps);
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND, s_overlay_src);
		s_dev->SetRenderState(D3DRS_DESTBLEND, s_overlay_dst);
		s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		for (const auto &p : s_water_polys) {
			if (!p.overlay)
				continue;
			s_dev->SetTexture(1, p.overlay);
			s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_water_verts[p.first], sizeof(WaterVertex));
		}
	}
	if (s_mode == Mode::DebugSolid) {
		// calibration quad in game screen coordinates: must cover x 100..540, y 200..400
		// calibration quad: 20%..80% of the game's view in x, 40%..80% in y (via dxwrapper's mapping)
		const float gw = 460.0f, gh = 343.0f, gx0 = 8.0f, gy0 = 8.0f; // game 3D view rect (8,8)-(468,351)
		const float sc[4] = {};
		s_dev->SetPixelShaderConstantF(8, sc, 1);
		const WaterVertex q[4] = {{gx0 + gw * 0.2f, gy0 + gh * 0.4f, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {gx0 + gw * 0.8f, gy0 + gh * 0.4f, 0.5f, 1, 0xFFFFFFFF, 1, 0},
		                          {gx0 + gw * 0.8f, gy0 + gh * 0.8f, 0.5f, 1, 0xFFFFFFFF, 1, 1}, {gx0 + gw * 0.2f, gy0 + gh * 0.8f, 0.5f, 1, 0xFFFFFFFF, 0, 1}};
		s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(WaterVertex));
	}
	s_state->Apply();
	if (prev_rt) {
		s_dev->SetRenderTarget(0, prev_rt);
		prev_rt->Release();
	}
	s_water_drawn_this_frame = true;
	static DWORD last = 0;
	if (GetTickCount() - last > 5000) {
		last = GetTickCount();
		float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
		for (const auto &v : s_water_verts) {
			ymin = v.y < ymin ? v.y : ymin; ymax = v.y > ymax ? v.y : ymax;
			xmin = v.x < xmin ? v.x : xmin; xmax = v.x > xmax ? v.x : xmax;
		}
		size_t layers = 0;
		for (const auto &p : s_water_polys) layers += p.overlay != nullptr;
		log_info("mm7fx dbg: shore layers in batch: %zu, overlay ps %p", layers, (void *)s_overlay_ps);
		log_info("mm7fx dbg: GrayFace map x*%.4f%+.1f y*%.4f%+.1f", s_gf_mul[0], s_gf_add[0], s_gf_mul[1], s_gf_add[1]);
		log_info("mm7fx dbg: batch %zu polys %zu verts, x [%.0f..%.0f] y [%.0f..%.0f], viewport %lu,%lu %lux%lu, RT %ux%u, dxw off (%.2f %.2f %.2f) mul (%.6f %.6f %.3f)",
			s_water_polys.size(), s_water_verts.size(), xmin, xmax, ymin, ymax, vp.X, vp.Y, vp.Width, vp.Height, rd.Width, rd.Height,
			s_dxw_off[0], s_dxw_off[1], s_dxw_off[2], s_dxw_mul[0], s_dxw_mul[1], s_dxw_mul[2]);
	}
}

// ---- hooks -----------------------------------------------------------------------------------

using DrawTerrainPolygonFn = void(__fastcall *)(void *self, void *edx, uint32_t n, uint8_t *poly, void *tex, int transparent, int clamp);
static DrawTerrainPolygonFn s_orig_draw_terrain_polygon = nullptr;

static void __fastcall hook_draw_terrain_polygon(void *self, void *edx, uint32_t n, uint8_t *poly, void *tex, int transparent, int clamp)
{
	s_current_poly = poly;
	s_current_n = n;
	s_poly_captured = false;
	mirror_begin(n);
	s_orig_draw_terrain_polygon(self, edx, n, poly, tex, transparent, clamp);
	mirror_end(poly);
	// TL array now holds this tile's final vertices, independent of when draw events fired.
	if (s_poly_captured && mirror_wanted())
		capture_water_poly();
	if (s_poly_tex) {
		s_poly_tex->Release();
		s_poly_tex = nullptr;
	}
	if (s_poly_overlay) {
		s_poly_overlay->Release();
		s_poly_overlay = nullptr;
	}
	s_poly_draws = 0;
	s_current_poly = nullptr;
}

using DrawBuildingsFn = void(__fastcall *)(void *self, void *edx);
static DrawBuildingsFn s_orig_draw_buildings = nullptr;

static void __fastcall hook_draw_buildings(void *self, void *edx)
{
	s_sky_pending = mirror_wanted(); // sky is drawn, buildings and terrain follow: copy at their first draw
	s_orig_draw_buildings(self, edx);
}

using DrawPolygonFn = void(__fastcall *)(void *self, void *edx, uint32_t n, uint8_t *poly, void *face, void *tex);
static DrawPolygonFn s_orig_draw_polygon = nullptr;

static void __fastcall hook_draw_polygon(void *self, void *edx, uint32_t n, uint8_t *poly, void *face, void *tex)
{
	mirror_begin(n);
	s_orig_draw_polygon(self, edx, n, poly, face, tex);
	mirror_end(poly);
}

using EndSceneFn = void(__fastcall *)(void *self, void *edx);
static EndSceneFn s_orig_end_scene = nullptr;

static void __fastcall hook_end_scene(void *self, void *edx)
{
	if (*reinterpret_cast<int *>(static_cast<uint8_t *>(self) + mm7::RenderSceneDepth) == 1) {
		// Vanilla order is sky -> buildings -> terrain, so water goes after all terrain, before billboards.
		if (s_dev && mirror_wanted()) {
			draw_water_batch();
			// Full 3D scene: next frame's reflection source.
			s_scene_ready = copy_scene();
		}
		release_captured();
		mirror_release_captured();
	}
	s_orig_end_scene(self, edx);
}

// ---- ReShade events --------------------------------------------------------------------------

static bool is_water(const uint8_t *poly)
{
	return *reinterpret_cast<const int16_t *>(poly + mm7::PolygonFlags) & (mm7::TileWater | mm7::TileShore);
}

static uint32_t s_dbg_left = 0; // diagnostic lines left for the current 5 s window

static bool on_draw(command_list *cmd, uint32_t vertex_count, uint32_t, uint32_t first_vertex, uint32_t)
{
	++s_draws;
	if (!s_dev)
		s_dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_device()->get_native()));
	mirror_on_draw();
	if (!s_current_poly)
		return false;
	++s_terrain_draws;
	if (s_mode == Mode::HideTerrain)
		return true;
	if (!is_water(s_current_poly))
		return false;
	++s_water_draws;
	switch (s_mode) {
	case Mode::DebugSolid:
	case Mode::DebugReflection:
	case Mode::Effect:
		if (s_dbg_left) {
			--s_dbg_left;
			const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
			log_info("mm7fx dbg: draw vc=%u first=%u captured=%d n=%u grid=(%d,%d) tl0=(%.1f,%.1f,%.4f) tl1=(%.1f,%.1f) tl2=(%.1f,%.1f)",
				vertex_count, first_vertex, s_poly_captured, s_current_n, (int)(int8_t)s_current_poly[0x5D], (int)(int8_t)s_current_poly[0x5C],
				tl[0].x, tl[0].y, tl[0].z, tl[1].x, tl[1].y, tl[2].x, tl[2].y);
		}
		if (s_poly_draws++ == 0) {
			// inside the draw: dxwrapper's per-draw viewport, target and fix-up constants are live right now
			s_dev->GetViewport(&s_game_vp);
			if (!s_game_rt)
				s_dev->GetRenderTarget(0, &s_game_rt);
			s_dev->GetVertexShaderConstantF(0, s_dxw_off, 1);
			s_dev->GetVertexShaderConstantF(1, s_dxw_mul, 1);
			update_grayface_scale(s_current_n);
			// Shore tiles come as two DrawTerrainPolygon calls: the water layer, then the shore texture alpha-blended.
			DWORD blend = FALSE;
			s_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			if (blend && (*reinterpret_cast<const int16_t *>(s_current_poly + mm7::PolygonFlags) & mm7::TileShore)) {
				s_dev->GetTexture(0, &s_poly_overlay);
				s_dev->GetRenderState(D3DRS_SRCBLEND, &s_overlay_src);
				s_dev->GetRenderState(D3DRS_DESTBLEND, &s_overlay_dst);
			} else {
				s_dev->GetTexture(0, &s_poly_tex); // base pass texture (water layer)
			}
		}
		static int shore_dbg = 3;
		if (shore_dbg > 0 && (*reinterpret_cast<const int16_t *>(s_current_poly + mm7::PolygonFlags) & mm7::TileShore)) {
			IDirect3DBaseTexture9 *t = nullptr;
			DWORD blend = 0, src = 0, dst = 0, top = 0;
			s_dev->GetTexture(0, &t);
			s_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			s_dev->GetRenderState(D3DRS_SRCBLEND, &src);
			s_dev->GetRenderState(D3DRS_DESTBLEND, &dst);
			s_dev->GetTextureStageState(0, D3DTSS_COLOROP, &top);
			log_info("mm7fx dbg: shore poly draw #%u tex=%p blend=%lu src=%lu dst=%lu colorop=%lu flags=%04x",
				s_poly_draws, (void *)t, blend, src, dst, top, *reinterpret_cast<const uint16_t *>(s_current_poly + mm7::PolygonFlags));
			if (t) t->Release();
			if (s_poly_draws >= 3) --shore_dbg;
		}
		if (!s_scene_ready || s_init_failed)
			return false; // effect not available yet (first frame): vanilla water
		s_poly_captured = true; // vertices captured when DrawTerrainPolygon returns
		return true; // drawn by draw_water_batch() at the end of the scene; vanilla passes skipped
	case Mode::HideWater:
		return true;
	default:
		return false;
	}
}

static bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	return on_draw(cmd, 0, 0, 0, 0);
}

static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	++s_frame;
	const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
	if (f10 && !s_f10_was_down) {
		s_mode = static_cast<Mode>((static_cast<int>(s_mode) + 1) % 4);
		static const char *const names[] = {"water effect", "vanilla", "hide water", "hide all terrain"};
		log_info("mm7fx: mode = %s", names[static_cast<int>(s_mode)]);
	}
	s_f10_was_down = f10;

	// Scriptable override: mm7fx_mode.txt in the game folder holds the mode number (0..5; 4 = debug solid,
	// 5 = reflection only); checked twice a second.
	static DWORD last_poll = 0;
	if (GetTickCount() - last_poll > 500) {
		last_poll = GetTickCount();
		if (FILE *f = std::fopen("mm7fx_mode.txt", "r")) {
			int m = -1;
			if (std::fscanf(f, "%d", &m) == 1 && m >= 0 && m <= 5 && m != static_cast<int>(s_mode)) {
				s_mode = static_cast<Mode>(m);
				log_info("mm7fx: mode = %d (from mm7fx_mode.txt)", m);
			}
			std::fclose(f);
		}
	}

	const DWORD now = GetTickCount();
	if (now - s_last_log >= 5000) {
		s_last_log = now;
		s_dbg_left = 2;
		log_info("mm7fx: frame %u: %u draws, %u terrain, %u water, horizon y %.1f px, mode %d, water z [%.5f..%.5f] rhw [%.6f..%.6f], game zenable %lu zfunc %lu zwrite %lu",
			s_frame, s_draws, s_terrain_draws, s_water_draws, s_horizon_px, static_cast<int>(s_mode), s_zmin, s_zmax, s_rmin, s_rmax,
			s_game_zenable, s_game_zfunc, s_game_zwrite);
		s_zmin = s_rmin = 9; s_zmax = s_rmax = -9;
	}
	s_draws = s_water_draws = s_terrain_draws = 0;
	if (!s_water_polys.empty() && !s_water_drawn_this_frame && s_mode == Mode::Effect) {
		static DWORD warned = 0;
		if (GetTickCount() - warned > 5000) { warned = GetTickCount(); log_info("mm7fx: water captured but not drawn this frame"); }
	}
	s_water_drawn_this_frame = false;
	release_captured(); // safety: never carry captures across frames
	mirror_release_captured();
}

static void on_destroy_device(device *)
{
	release_captured();
	release_mirror_resources();
	if (s_state) s_state->Release();
	if (s_water_ps) s_water_ps->Release();
	if (s_overlay_ps) s_overlay_ps->Release();
	s_overlay_ps = nullptr;
	if (s_water_vs) s_water_vs->Release();
	if (s_water_decl) s_water_decl->Release();
	s_water_vs = nullptr;
	s_water_decl = nullptr;
	if (s_scene_tex) s_scene_tex->Release();
	s_scene_ready = false;
	s_state = nullptr;
	s_water_ps = nullptr;
	s_scene_tex = nullptr;
	s_dev = nullptr;
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
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		if (install_detour(mm7::DrawTerrainPolygon, mm7::DrawTerrainPolygonPrologue,
				reinterpret_cast<void *>(hook_draw_terrain_polygon), reinterpret_cast<void **>(&s_orig_draw_terrain_polygon)))
			log_info("mm7fx: hooked DrawTerrainPolygon");
		else
			log_info("mm7fx: DrawTerrainPolygon prologue mismatch, water disabled");
		if (install_detour(mm7::DrawBuildingsD3D, mm7::DrawBuildingsD3DPrologue,
				reinterpret_cast<void *>(hook_draw_buildings), reinterpret_cast<void **>(&s_orig_draw_buildings)))
			log_info("mm7fx: hooked DrawBuildingsD3D");
		else
			log_info("mm7fx: DrawBuildingsD3D prologue mismatch");
		if (install_detour(mm7::DrawPolygon, mm7::DrawPolygonPrologue,
				reinterpret_cast<void *>(hook_draw_polygon), reinterpret_cast<void **>(&s_orig_draw_polygon)))
			log_info("mm7fx: hooked DrawPolygon");
		else
			log_info("mm7fx: DrawPolygon prologue mismatch, buildings not mirrored");
		if (install_detour(mm7::EndSceneBillboards, mm7::EndSceneBillboardsPrologue,
				reinterpret_cast<void *>(hook_end_scene), reinterpret_cast<void **>(&s_orig_end_scene)))
			log_info("mm7fx: hooked DrawBillboards_And_..._EndScene");
		else
			log_info("mm7fx: EndScene prologue mismatch, water effect disabled");
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
