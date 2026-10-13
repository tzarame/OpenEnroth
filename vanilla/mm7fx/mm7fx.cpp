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
struct WaterVertex { float x, y, z, rhw; uint32_t diffuse; float u, v; float wx, wy; };
static const D3DVERTEXELEMENT9 s_water_decl_elems[] = {
	{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
	{0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
	{0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
	{0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
	D3DDECL_END()};
// Shore tiles are drawn by vanilla in two layers: water first, then the shore texture alpha-blended on top.
// tex = water layer, overlay = shore layer (null for pure water).
struct WaterPoly { IDirect3DBaseTexture9 *tex; uint32_t first, count; bool shore; IDirect3DBaseTexture9 *overlay; float z; };
static std::vector<WaterVertex> s_water_verts;
static std::vector<WaterPoly> s_water_polys;

// horizon fit: screen y = a + b * (v_vert / v_fwd); camera "up" from world-axis deltas of tile corners
struct Fit { double n = 0, st = 0, sy = 0, stt = 0, sty = 0; float up[3] = {}; bool have_up = false; };
// Camera basis in view space (images of world X, Y, Z axes) + camera world position, from flat water tiles;
// kept across frames so every water vertex can be given world coordinates (world-anchored waves).
static float s_bx[3], s_by[3], s_bz[3], s_cam[3], s_water_z = 0;
static bool s_basis_ok = false;
static uint32_t s_fit_vertex = 0;
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
	float pz = 0;
	for (uint32_t i = 0; i < n; ++i) pz += sv[i].wz;
	WaterPoly wp = {s_poly_tex, static_cast<uint32_t>(s_water_verts.size()), n, shore && !layer && false, s_poly_overlay, pz / n};
	s_poly_tex = nullptr; // references move to the poly
	s_poly_overlay = nullptr;
	for (uint32_t i = 0; i < n; ++i) {
		const auto &t = tl[i];
		s_zmin = t.z < s_zmin ? t.z : s_zmin; s_zmax = t.z > s_zmax ? t.z : s_zmax;
		s_rmin = t.rhw < s_rmin ? t.rhw : s_rmin; s_rmax = t.rhw > s_rmax ? t.rhw : s_rmax;
		// -0.5: D3D9 rasterizes pixel centers at integer coords, D3D7 TL vertices at +0.5
		float wx = 0, wy = 0;
		if (s_basis_ok) { // world = camera + basis^T * view
			const float v[3] = {sv[i].vx, sv[i].vy, sv[i].vz};
			wx = s_cam[0] + v[0] * s_bx[0] + v[1] * s_bx[1] + v[2] * s_bx[2];
			wy = s_cam[1] + v[0] * s_by[0] + v[1] * s_by[1] + v[2] * s_by[2];
		}
		s_water_verts.push_back({t.x, t.y, t.z, t.rhw, t.diffuse, t.u, t.v, wx, wy});
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
				if (!gx && std::fabs(dwx) > 100 && std::fabs(dwy) < 1 && std::fabs(dvl - std::fabs(dwx)) < 0.01f * std::fabs(dwx)) { for (int k = 0; k < 3; ++k) rx[k] = dv[k] / dwx; gx = true; s_fit_vertex = i; }
				if (!gy && std::fabs(dwy) > 100 && std::fabs(dwx) < 1 && std::fabs(dvl - std::fabs(dwy)) < 0.01f * std::fabs(dwy)) { for (int k = 0; k < 3; ++k) ry[k] = dv[k] / dwy; gy = true; }
			}
		if (gx && gy) {
			s_fit.up[0] = rx[1] * ry[2] - rx[2] * ry[1];
			s_fit.up[1] = rx[2] * ry[0] - rx[0] * ry[2];
			s_fit.up[2] = rx[0] * ry[1] - rx[1] * ry[0];
			s_fit.have_up = true;
			// camera world position from a vertex whose world coords are trustworthy (unclipped: |view| == |world| deltas)
			const float ul = std::sqrt(s_fit.up[0] * s_fit.up[0] + s_fit.up[1] * s_fit.up[1] + s_fit.up[2] * s_fit.up[2]);
			if (ul > 1e-6f) {
				for (int k = 0; k < 3; ++k) { s_bx[k] = rx[k]; s_by[k] = ry[k]; s_bz[k] = s_fit.up[k] / ul; }
				const auto &q = sv[s_fit_vertex];
				const float v[3] = {q.vx, q.vy, q.vz};
				s_cam[0] = q.wx - (v[0] * s_bx[0] + v[1] * s_bx[1] + v[2] * s_bx[2]);
				s_cam[1] = q.wy - (v[0] * s_by[0] + v[1] * s_by[1] + v[2] * s_by[2]);
				s_cam[2] = q.wz - (v[0] * s_bz[0] + v[1] * s_bz[1] + v[2] * s_bz[2]);
				s_water_z = q.wz;
				s_basis_ok = true;
			}
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
struct VSIn { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float2 world : TEXCOORD1; };
struct VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float2 world : TEXCOORD1; };
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
	o.world = i.world;
	return o;
}
)";

// shore layer drawn over our water: plain texture * vertex colour, alpha from the texture (like vanilla)
static const char s_overlay_ps_src[] = R"(
sampler2D layerTex : register(s1);    // POINT: colour without bleeding from transparent texels
sampler2D layerTexLin : register(s4); // LINEAR: smooth alpha edge
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
	float4 t = float4(tex2D(layerTex, uv).rgb, tex2D(layerTexLin, uv).a);
	// sampled with POINT filtering (see the overlay pass): the transparent texels' colour must not bleed into
	// the edge (linear filtering gave dark or bright outlines whatever the blend mode)
	return float4(t.rgb * color.rgb, t.a); // vertex alpha is not meaningful for terrain
}
)";

static const char s_water_ps_src[] = R"(
sampler2D sceneTex : register(s0);
sampler2D waterTex : register(s1);
sampler2D mirrorTex : register(s2);
sampler2D waveTex : register(s3);   // tileable wave normal map (xy in rg)
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
float4 cam : register(c10);   // camera world position xyz, w = 1 if world coords are valid
float4 sun : register(c11);   // world direction to the sun xyz, w = sun intensity (0 at night)
float4 wat : register(c12);   // x = water surface world z, y = wave strength, z = sun specular power
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
float2 wave(float2 p, float2 dir, float speed, float scale, float time)
{
	return tex2D(waveTex, p / scale + dir * time * speed).rg * 2.0 - 1.0;
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float2 world : TEXCOORD1, float2 vpos : VPOS) : COLOR
{
	float time = params.x;
	float2 gp = to_game(vpos + 0.5);
	float y = gp.y;                                    // game screen px

	// --- waves: three world-anchored layers of the normal map, detail fades with distance ---
	float3 P = float3(world, wat.x);
	float3 toCam = cam.xyz - P;
	float dist = length(toCam);
	float3 V = toCam / max(dist, 1.0);
	float detail = saturate(1.0 - dist / 7000.0);
	float2 nxy = wave(world, float2(0.71, 0.70), 0.020, 1300.0, time) * 0.55
	           + wave(world, float2(-0.86, 0.50), 0.011, 3100.0, time) * 0.45
	           + wave(world, float2(0.20, -0.98), 0.045, 430.0, time) * 0.35 * detail;
	nxy *= wat.y * cam.w;
	float3 N = normalize(float3(nxy, 1.0));

	// --- reflection (planar mirror, angular sky fallback), distorted by the wave normals ---
	float2 rip = nxy * 0.03 * saturate(1800.0 / dist);
	float t = (y - params.y) / params.z;               // tan of the view-space vertical angle
	float den = 1.0 + params.w * t;
	float tr = (params.w - t) / den;                   // mirrored about the horizon angle
	float yr = params.y + params.z * tr;               // reflected point, game px
	// steep views: the reflected ray points (almost) at the zenith, the formula degenerates (den -> 0) and
	// stretched clouds showed as bright rays; such rays and any not above the horizon use the zenith fallback
	float valid = saturate(den * 10.0) * saturate((extra.y - yr) * 0.05);
	float2 ruv = to_rt_uv(float2(gp.x, yr)) + rip;
	// valid image = the game's 3D view; the render target around it is not drawn (black)
	float2 vlo = to_rt_uv(view.xy), vhi = to_rt_uv(view.zw);
	float2 lo = min(vlo, vhi), hi = max(vlo, vhi) - rt.xy;
	float inside = saturate((ruv.y - lo.y) * 8.0) * valid; // reflected ray left the top of the view: no data
	float3 refl = tex2D(sceneTex, clamp(ruv, lo, hi)).rgb;
	// beyond the top of the view: a horizontally blurred sky colour from the top rows instead of a stretched
	// row (which showed as vertical streaks through the clouds)
	float ty = lo.y + 0.02;
	float3 skyTop = (tex2D(sceneTex, float2(clamp(ruv.x - 0.12, lo.x, hi.x), ty)).rgb + tex2D(sceneTex, float2(clamp(ruv.x - 0.06, lo.x, hi.x), ty)).rgb
	               + tex2D(sceneTex, float2(clamp(ruv.x, lo.x, hi.x), ty)).rgb + tex2D(sceneTex, float2(clamp(ruv.x + 0.06, lo.x, hi.x), ty)).rgb
	               + tex2D(sceneTex, float2(clamp(ruv.x + 0.12, lo.x, hi.x), ty)).rgb) * 0.2;
	refl = lerp(skyTop, refl, inside);
	// 1px cracks between mirrored polygons (T-junctions) let the sky through: fill from the neighbours
	float2 mc = clamp((vpos + 0.5) * rt.xy + rip, lo, hi);
	float4 mr = tex2D(mirrorTex, mc);
	float4 m1 = tex2D(mirrorTex, mc + float2(rt.x, 0.0)), m2 = tex2D(mirrorTex, mc - float2(rt.x, 0.0));
	float4 m3 = tex2D(mirrorTex, mc + float2(0.0, rt.y)), m4 = tex2D(mirrorTex, mc - float2(0.0, rt.y));
	float wsum = m1.a + m2.a + m3.a + m4.a;
	float3 nb = (m1.rgb * m1.a + m2.rgb * m2.a + m3.rgb * m3.a + m4.rgb * m4.a) / max(wsum, 1e-3);
	float fill = (1.0 - mr.a) * saturate(wsum - 1.0); // at least two covered neighbours: a crack, not an edge
	mr = float4(lerp(mr.rgb, nb, fill), max(mr.a, fill));
	refl = lerp(refl, mr.rgb, mr.a * mir.x);
	inside = lerp(inside, 1.0, mir.x);
	if (mir.y > 0.5)
		return float4(refl, 1.0);

	// --- water body: the game's water texture deepened towards a dark blue-green ---
	float m = shore.x > 0.5 ? water_mask(tex2D(waterTex, uv).rgb) : 1.0;
	float3 tex = tex2D(waterTex, uv + rip * 3.0 * m).rgb * color.rgb;
	float3 deep = float3(0.035, 0.11, 0.16) * (0.4 + 0.6 * sun.w);
	float3 body = lerp(deep, tex, 0.55);

	// --- Fresnel (Schlick, water F0 = 0.02): looking down you see the water, at grazing angles the sky ---
	float NdV = saturate(dot(N, V));
	float fres = 0.02 + 0.98 * pow(1.0 - NdV, 5.0);
	float k = saturate(fres * extra.x * 1.6) * inside * m;
	float lb = dot(tex, float3(0.299, 0.587, 0.114));
	float3 tint = tex / max(lb, 0.05);
	float3 wrefl = refl * lerp(float3(1.0, 1.0, 1.0), tint, 0.3);
	float3 c = lerp(body, wrefl, k);

	// --- sun glints ---
	float3 R = reflect(-V, N);
	float spec = pow(saturate(dot(R, sun.xyz)), wat.z) * sun.w * m * cam.w;
	c += spec * float3(1.0, 0.92, 0.78) * 2.5;

	float3 dbg = shore.x > 0.5 ? float3(0.0, 1.0, 1.0) : float3(1.0, 0.0, 1.0);
	return float4(lerp(c, dbg, extra.w), 1.0); // extra.w = 1: debug solid magenta (water) / cyan (shore)
}
)";

static IDirect3DPixelShader9 *s_water_ps = nullptr;
static IDirect3DPixelShader9 *s_overlay_ps = nullptr;
static IDirect3DTexture9 *s_wave_tex = nullptr; // procedural tileable wave normal map
static IDirect3DVertexShader9 *s_water_vs = nullptr;
static IDirect3DVertexDeclaration9 *s_water_decl = nullptr;
static IDirect3DTexture9 *s_scene_tex = nullptr;
static UINT s_scene_w = 0, s_scene_h = 0;
static IDirect3DStateBlock9 *s_state = nullptr;
static bool s_init_failed = false;

// Tileable wave normal map: sum of sines with integer wave vectors (so it wraps), amplitudes falling with frequency.
static bool create_wave_texture()
{
	constexpr int S = 256;
	if (FAILED(s_dev->CreateTexture(S, S, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_wave_tex, nullptr)))
		return false;
	struct W { int kx, ky; float a, ph; };
	W w[40];
	uint32_t seed = 0x9E3779B9u;
	auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
	for (auto &x : w) {
		const float ang = rnd() * 6.2831853f, f = 1.0f + rnd() * rnd() * 9.0f;
		x.kx = static_cast<int>(std::lround(std::cos(ang) * f));
		x.ky = static_cast<int>(std::lround(std::sin(ang) * f));
		if (!x.kx && !x.ky) x.kx = 1;
		x.a = 1.0f / (0.6f + std::sqrt(float(x.kx * x.kx + x.ky * x.ky)));
		x.ph = rnd() * 6.2831853f;
	}
	std::vector<float> h(S * S);
	for (int y = 0; y < S; ++y)
		for (int x = 0; x < S; ++x) {
			float v = 0;
			for (const auto &q : w)
				v += q.a * std::sin(6.2831853f * (q.kx * x + q.ky * y) / S + q.ph);
			h[y * S + x] = v;
		}
	std::vector<uint32_t> lvl(S * S);
	for (int y = 0; y < S; ++y)
		for (int x = 0; x < S; ++x) {
			const float dx = h[y * S + (x + 1) % S] - h[y * S + (x + S - 1) % S];
			const float dy = h[((y + 1) % S) * S + x] - h[((y + S - 1) % S) * S + x];
			float nx = -dx * 0.9f, ny = -dy * 0.9f, nz = 1.0f;
			const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
			nx /= l; ny /= l;
			const auto enc = [](float c) { return static_cast<uint32_t>(std::lround((c * 0.5f + 0.5f) * 255.0f)); };
			lvl[y * S + x] = 0xFF000000u | (enc(nx) << 16) | (enc(ny) << 8) | enc(nz / l);
		}
	// upload level 0 and box-filtered mips
	for (DWORD level = 0, size = S; level < s_wave_tex->GetLevelCount(); ++level, size = size > 1 ? size / 2 : 1) {
		D3DLOCKED_RECT lr;
		if (FAILED(s_wave_tex->LockRect(level, &lr, nullptr, 0)))
			return false;
		for (DWORD y = 0; y < size; ++y)
			std::memcpy(static_cast<uint8_t *>(lr.pBits) + y * lr.Pitch, &lvl[y * size], size * 4);
		s_wave_tex->UnlockRect(level);
		if (size == 1)
			break;
		std::vector<uint32_t> next((size / 2) * (size / 2));
		for (DWORD y = 0; y < size / 2; ++y)
			for (DWORD x = 0; x < size / 2; ++x) {
				uint32_t acc[4] = {};
				for (int k = 0; k < 4; ++k) {
					const uint32_t c = lvl[(2 * y + k / 2) * size + 2 * x + k % 2];
					for (int ch = 0; ch < 4; ++ch) acc[ch] += (c >> (8 * ch)) & 0xFF;
				}
				next[y * (size / 2) + x] = (acc[0] / 4) | ((acc[1] / 4) << 8) | ((acc[2] / 4) << 16) | ((acc[3] / 4) << 24);
			}
		lvl.swap(next);
	}
	return true;
}

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
	if (SUCCEEDED(hr) && !create_wave_texture()) hr = E_FAIL;
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
// Water can lie at several heights (sea, inland ponds): the mirror plane uses the most common level in view,
// water at other levels gets the angular (sky) reflection instead.
struct WaterLevel { float z; double sum[3]; uint32_t n; };
static std::vector<WaterLevel> s_mir_levels;
static float s_mirror_level_z = 0;
static bool s_sky_pending = false, s_sky_ready = false;
static IDirect3DTexture9 *s_sky_tex = nullptr;     // frame right after the sky pass (sky only)
static IDirect3DTexture9 *s_mirror_tex = nullptr;  // mirrored geometry, alpha = coverage
static IDirect3DSurface9 *s_mirror_depth = nullptr;
static IDirect3DVertexShader9 *s_mirror_vs = nullptr;
static IDirect3DPixelShader9 *s_mirror_ps = nullptr;
static IDirect3DVertexDeclaration9 *s_mirror_decl = nullptr;
static bool s_mirror_init_failed = false;

// Billboards (trees, monsters, decorations) are drawn by the game after our mirror pass, from its D3D list
// (count 0xF1B298, entries of 0x9C bytes at 0xEF5138: +0 D3D7 texture, +4 vertex count, +8 TL quad[4]).
// We capture them while the game draws them (D3D9 texture from the draw, vertices from the list entry) and add them
// to the next frame's mirror pass.
namespace mm7 {
constexpr uintptr_t BillboardCount = 0xF1B298;
constexpr uintptr_t BillboardList = 0xEF5138;
constexpr size_t BillboardStride = 0x9C;
}
static bool s_bb_capture = false;
static uint32_t s_bb_index = 0, s_bb_count = 0;
static std::vector<MirrorPoly> s_bb_polys;
static std::vector<MirrorVertex> s_bb_verts;
static float s_mir_last_fit[4] = {}; // ax, bx, ay, by of the last mirror pass
static bool s_mir_last_fit_ok = false;

static void bb_release()
{
	for (auto &p : s_bb_polys)
		if (p.tex) p.tex->Release();
	s_bb_polys.clear();
	s_bb_verts.clear();
}

// on_draw while the game draws its billboard list (back to front: entry count-1 first)
static void bb_on_draw()
{
	if (s_bb_index >= s_bb_count || !s_mir_last_fit_ok)
		return;
	const uint8_t *e = reinterpret_cast<const uint8_t *>(mm7::BillboardList) + (s_bb_count - 1 - s_bb_index) * mm7::BillboardStride;
	++s_bb_index;
	const uint32_t n = *reinterpret_cast<const uint32_t *>(e + 4);
	if (n < 3 || n > 4)
		return;
	const auto *tl = reinterpret_cast<const mm7::TLVertex *>(e + 8);
	const float ax = s_mir_last_fit[0], bx = s_mir_last_fit[1], ay = s_mir_last_fit[2], by = s_mir_last_fit[3];
	MirrorPoly p = {nullptr, static_cast<uint32_t>(s_bb_verts.size()), n};
	for (uint32_t i = 0; i < n; ++i) {
		if (tl[i].rhw <= 0)
			return;
		const float vx = 1.0f / tl[i].rhw; // billboard rhw = 1 / view depth
		s_bb_verts.push_back({vx, (tl[i].x - ax) * vx / bx, (tl[i].y - ay) * vx / by, tl[i].diffuse, tl[i].u, tl[i].v});
	}
	s_dev->GetTexture(0, &p.tex);
	if (p.tex)
		s_bb_polys.push_back(p);
	else
		s_bb_verts.resize(p.first);
}

constexpr float MirrorMaxDist = 7000; // forward depth beyond which geometry is not mirrored
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
	s_mir_levels.clear();
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

static void fog_capture_poly(uint32_t n, bool terrain, bool water); // volumetric fog section

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
	fog_capture_poly(n, s_current_poly != nullptr, water && s_current_poly);
	// shore tiles (water layer + shore layer at water level) must not be mirrored either: their flat water layer
	// would cover the mirrored landscape near the coast
	if (!water && (flags & mm7::TileShore)) {
	} else if (water) {
		float z = 0;
		for (uint32_t i = 0; i < n; ++i) z += sv[i].wz;
		z /= n;
		WaterLevel *lv = nullptr;
		for (auto &l : s_mir_levels)
			if (std::fabs(l.z - z) < 24.0f) { lv = &l; break; }
		if (!lv) { s_mir_levels.push_back({z, {0, 0, 0}, 0}); lv = &s_mir_levels.back(); }
		for (uint32_t i = 0; i < n; ++i) {
			lv->sum[0] += sv[i].vx; lv->sum[1] += sv[i].vy; lv->sum[2] += sv[i].vz;
		}
		lv->n += n;
		s_mir_water_cnt += n;
	} else if (s_mir_have_verts && s_mir_cur_tex && [&] {
		// far geometry reflects tiny and under ripples: leave it to the sky reflection (big perf win)
		for (uint32_t i = 0; i < n; ++i)
			if (s_mir_cur[i].x < MirrorMaxDist) return true;
		return false; }()) {
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
	s_mir_last_fit[0] = ax; s_mir_last_fit[1] = bx; s_mir_last_fit[2] = ay; s_mir_last_fit[3] = by;
	s_mir_last_fit_ok = true;
	// previous frame's billboards join the mirrored geometry (references move to s_mir_polys)
	for (auto p : s_bb_polys) {
		p.first += static_cast<uint32_t>(s_mir_verts.size());
		s_mir_polys.push_back(p);
	}
	s_mir_verts.insert(s_mir_verts.end(), s_bb_verts.begin(), s_bb_verts.end());
	s_bb_polys.clear();
	s_bb_verts.clear();
	float nrm[3] = {s_fit.up[0], s_fit.up[1], s_fit.up[2]};
	const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
	if (len < 1e-6f)
		return false;
	const WaterLevel *dom = nullptr;
	for (const auto &l : s_mir_levels)
		if (!dom || l.n > dom->n) dom = &l;
	if (!dom || !dom->n)
		return false;
	s_mirror_level_z = dom->z;
	float d = 0;
	for (int k = 0; k < 3; ++k) {
		nrm[k] /= len;
		d += nrm[k] * static_cast<float>(dom->sum[k] / dom->n);
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
	// full resolution: at half resolution the reflection looked visibly blurrier
	const UINT mw = rd.Width, mh = rd.Height;
	if (!s_mirror_tex || md.Width != mw || md.Height != mh) {
		if (s_mirror_tex) s_mirror_tex->Release();
		if (s_mirror_depth) s_mirror_depth->Release();
		s_mirror_tex = nullptr;
		s_mirror_depth = nullptr;
		if (FAILED(s_dev->CreateTexture(mw, mh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_mirror_tex, nullptr)) ||
		    FAILED(s_dev->CreateDepthStencilSurface(mw, mh, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &s_mirror_depth, nullptr))) {
			log_info("mm7fx: mirror target %ux%u failed", mw, mh);
			if (s_mirror_tex) s_mirror_tex->Release();
			s_mirror_tex = nullptr;
			s_mirror_init_failed = true; // don't retry every frame
			return false;
		}
		log_info("mm7fx: mirror target %ux%u", mw, mh);
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
	{
		// sun from game time: rises in the east (+X) at 6:00, culminates in the south (-Y), sets in the west at 18:00
		const int hour = *reinterpret_cast<const int32_t *>(0xACD554), minute = *reinterpret_cast<const int32_t *>(0xACD558);
		const float tday = (hour + minute / 60.0f - 6.0f) / 12.0f * 3.14159265f; // 0 at sunrise, pi at sunset
		float sx = std::cos(tday), sy = -0.45f * std::sin(tday), sz = std::sin(tday);
		const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
		sx /= sl; sy /= sl; sz /= sl;
		const float sun_i = sz > 0 ? std::min(1.0f, sz * 4.0f) : 0.0f;
		const float wc[12] = {s_cam[0], s_cam[1], s_cam[2], s_basis_ok ? 1.0f : 0.0f,
		                      sx, sy, sz, sun_i,
		                      s_water_z, 0.55f, 260.0f, 0.0f};
		s_dev->SetPixelShaderConstantF(10, wc, 3);
		s_dev->SetTexture(3, s_wave_tex);
		s_dev->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		s_dev->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		s_dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		s_dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		s_dev->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	}
	s_dev->SetVertexShaderConstantF(0, s_dxw_off, 1);
	s_dev->SetVertexShaderConstantF(1, s_dxw_mul, 1);
	const float gfc[4] = {s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1]};
	s_dev->SetVertexShaderConstantF(2, gfc, 1);
	s_dev->SetPixelShaderConstantF(6, gfc, 1);
	s_dev->SetPixelShaderConstantF(7, mirc, 1);
	// same depth setup the game used for its water (it may be a W-buffer)
	s_dev->SetRenderState(D3DRS_ZENABLE, s_mode == Mode::DebugSolid ? D3DZB_FALSE : s_game_zenable); // debug: no depth test
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, s_game_zwrite);
	s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	// the vanilla water stays underneath as a crack filler at the very same depth: always win against it
	const float zbias = -0.0002f, zslope = -1.0f;
	s_dev->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD *>(&zbias));
	s_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<const DWORD *>(&zslope));
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
	int shore = -1, level_on = -1;
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
		// planar mirror only for water at the mirror plane's level; ponds at other heights: angular sky reflection
		const int on = planar && std::fabs(p.z - s_mirror_level_z) < 24.0f;
		if (on != level_on) {
			level_on = on;
			const float mc[4] = {float(on), s_mode == Mode::DebugReflection ? 1.0f : 0.0f, 0, 0};
			s_dev->SetPixelShaderConstantF(7, mc, 1);
		}
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_water_verts[p.first], sizeof(WaterVertex));
	}
	// shore layers on top of the water, blended like vanilla did
	if (s_mode == Mode::Effect) {
		s_dev->SetPixelShader(s_overlay_ps);
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND, s_overlay_src);
		s_dev->SetRenderState(D3DRS_DESTBLEND, s_overlay_dst);
		s_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		s_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		s_dev->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		s_dev->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		// CLAMP: with WRAP the linear alpha at a tile's edge mixed in the opposite edge of the shore texture
		// (thin brown dashed line along tile borders)
		s_dev->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		for (const auto &p : s_water_polys) {
			if (!p.overlay)
				continue;
			s_dev->SetTexture(1, p.overlay);
			s_dev->SetTexture(4, p.overlay);
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

// ---- volumetric height fog -------------------------------------------------------------------
// After the billboards (before the UI) the finished 3D view gets height fog that also hides the draw-distance edge.
//  1. depth: all captured geometry of this frame (terrain, water, shore, buildings from mirror_end; billboards from
//     bb_on_draw, alpha-tested) is re-rendered in view space into an R32F target holding forward depth (0 = sky);
//  2. fog: a quad over the game's RenderRect reconstructs every pixel's view ray from the fitted projection, turns it
//     into world space with the camera basis fitted from the soft vertices (world vs view positions of the same
//     vertices) and ray-marches density = Density * exp(-height above water / H) * (0.55 + 0.45 * noise);
//     blended as dst * T + inscatter * (1 - T). Sky pixels get the same march plus horizon haze.
// Toggle: Effect mode only, and mm7fx_fog.txt (0/1, default 1) in the game folder, polled like mm7fx_mode.txt.

namespace fogcfg { // MM7 units: a terrain tile is 512
constexpr float Density = 2.6e-4f;     // extinction per unit at water level, fully inside the fog
constexpr float HeightFalloff = 450;   // H of the dense ground layer (the thin haze above uses 2500, in the shader)
constexpr float MaxDist = 20000;       // march length cap (sky rays use all of it)
constexpr float FogStart = 1200;       // no fog closer than this ...
constexpr float FogRamp = 3500;        // ... full density from FogStart + FogRamp on
constexpr float NoiseScale = 1.0f / (128 * 900);  // noise texture (128 texels) uv per world unit: ~900-unit banks
constexpr float WindX = 90, WindY = 40; // units/s; the fine octave drifts 1.6x faster
constexpr float SunGlow = 0.45f;       // forward-scattering glow strength toward the sun
constexpr float SunG = 0.55f;          // Henyey-Greenstein anisotropy of the glow
constexpr float SkyHazeElev = 0.12f;   // sky: fully fogged at the horizon, clear from this sin(elevation) up
constexpr float EdgeStart = 0.7f;      // geometry: forced fully opaque from EdgeStart * far to far (draw-distance edge)
constexpr float SunNoonElev = 0.96f;   // sun elevation at noon, radians (~55 deg)
}
namespace mm7 {
constexpr uintptr_t GameHour = 0xACD554, GameMinute = 0xACD558; // int32
}

static bool s_fog_on = true;               // mm7fx_fog.txt
static std::vector<MirrorVertex> s_fog_tris; // this frame's opaque geometry, view space, triangle list
static bool s_fog_terrain = false;         // an outdoor terrain frame (indoors: no fog)
static IDirect3DSurface9 *s_fog_rt = nullptr; // game render target at the first captured draw (AddRef'd)
static D3DVIEWPORT9 s_fog_vp = {};
static float s_fog_dxw[8] = {};            // dxwrapper fix-up offset (c0) and multiplier (c1) at that draw
static bool s_fog_gf_done = false;
static float s_fog_proj[4] = {};           // ax, bx, ay, by of this frame
static bool s_fog_sky_ok = false;
static float s_fog_far = 0;                // largest terrain forward depth seen = draw distance
static float s_fog_base_z = 0;             // world z of the water plane (last seen)
// camera: least-squares fit view = R * (world - cam) from validated vertices
struct FogAcc { double sv[3] = {}, sw[3] = {}, svw[3][3] = {}, sww[3][3] = {}, wref[3] = {}; uint32_t cnt = 0; };
static FogAcc s_fog_acc;
static float s_fog_rx[3], s_fog_ry[3], s_fog_rz[3], s_fog_cam[3]; // view images of world X/Y/Z, camera world pos
static bool s_fog_basis_ok = false;

static IDirect3DVertexShader9 *s_fog_depth_vs = nullptr, *s_fog_quad_vs = nullptr;
static IDirect3DPixelShader9 *s_fog_depth_ps = nullptr, *s_fog_ps = nullptr;
static IDirect3DVertexDeclaration9 *s_fog_decl = nullptr;
static IDirect3DStateBlock9 *s_fog_state = nullptr;
static IDirect3DTexture9 *s_fog_noise = nullptr, *s_fog_depth_tex = nullptr;
static IDirect3DSurface9 *s_fog_ds = nullptr;
static bool s_fog_init_failed = false;

static bool fog_wanted()
{
	return s_mode == Mode::Effect && s_fog_on;
}
static bool shadows_wanted();  // sun shadows + AO section: also needs this frame's captures and linear depth
static bool shadows_debug();
static void shadows_render(const float sun[3]);
static bool fx_depth_wanted() { return fog_wanted() || shadows_wanted(); }

static void fog_poll_toggle()
{
	if (FILE *f = std::fopen("mm7fx_fog.txt", "r")) {
		int v = 1;
		if (std::fscanf(f, "%d", &v) == 1 && (v != 0) != s_fog_on) {
			s_fog_on = v != 0;
			log_info("mm7fx: fog %s (from mm7fx_fog.txt)", s_fog_on ? "on" : "off");
		}
		std::fclose(f);
	}
}

// from mirror_end, for every terrain/building polygon (soft vertices are final)
static void fog_capture_poly(uint32_t n, bool terrain, bool water)
{
	if (!fx_depth_wanted())
		return;
	const auto *sv = reinterpret_cast<const mm7::SoftVertex *>(mm7::SoftVertices);
	for (uint32_t k = 2; k < n; ++k)
		for (uint32_t i : {0u, k - 1, k})
			s_fog_tris.push_back({sv[i].vx, sv[i].vy, sv[i].vz, 0, 0, 0});
	if (terrain) {
		s_fog_terrain = true;
		for (uint32_t i = 0; i < n; ++i)
			s_fog_far = std::max(s_fog_far, sv[i].vx); // ponytail: never shrinks; reset if view distance is lowered at runtime
	}
	// basis samples only from polygons whose view and world edge lengths agree (clipped vertices keep stale world positions)
	for (uint32_t i = 1; i < n; ++i) {
		const float dv[3] = {sv[i].vx - sv[0].vx, sv[i].vy - sv[0].vy, sv[i].vz - sv[0].vz};
		const float dw[3] = {sv[i].wx - sv[0].wx, sv[i].wy - sv[0].wy, sv[i].wz - sv[0].wz};
		const float lv = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
		const float lw = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
		if (std::fabs(lv - lw) > 0.01f * lw + 1.0f)
			return;
	}
	FogAcc &a = s_fog_acc;
	if (!a.cnt) {
		a.wref[0] = sv[0].wx; a.wref[1] = sv[0].wy; a.wref[2] = sv[0].wz;
	}
	for (uint32_t i = 0; i < n; ++i) {
		const double v[3] = {sv[i].vx, sv[i].vy, sv[i].vz};
		const double w[3] = {sv[i].wx - a.wref[0], sv[i].wy - a.wref[1], sv[i].wz - a.wref[2]};
		for (int r = 0; r < 3; ++r) {
			a.sv[r] += v[r];
			a.sw[r] += w[r];
			for (int c = 0; c < 3; ++c) {
				a.svw[r][c] += v[r] * w[c];
				a.sww[r][c] += w[r] * w[c];
			}
		}
	}
	a.cnt += n;
	if (water)
		s_fog_base_z = sv[0].wz;
}

// R = Cvw * Cww^-1 (covariances), orthonormalised; cam = mean(w) - R^T mean(v)
static bool fog_solve_basis()
{
	const FogAcc &a = s_fog_acc;
	if (a.cnt < 16)
		return false;
	const double inv_n = 1.0 / a.cnt;
	double cvw[3][3], cww[3][3];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			cvw[r][c] = a.svw[r][c] * inv_n - a.sv[r] * inv_n * a.sw[c] * inv_n;
			cww[r][c] = a.sww[r][c] * inv_n - a.sw[r] * inv_n * a.sw[c] * inv_n;
		}
	const double ridge = 1e-6 * (cww[0][0] + cww[1][1] + cww[2][2]) + 1e-3; // flat views: no world-z spread
	for (int k = 0; k < 3; ++k)
		cww[k][k] += ridge;
	double inv[3][3];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) // cofactor (transposed)
			inv[c][r] = cww[(r + 1) % 3][(c + 1) % 3] * cww[(r + 2) % 3][(c + 2) % 3] - cww[(r + 1) % 3][(c + 2) % 3] * cww[(r + 2) % 3][(c + 1) % 3];
	const double det = cww[0][0] * inv[0][0] + cww[0][1] * inv[1][0] + cww[0][2] * inv[2][0];
	if (std::fabs(det) < 1e-12)
		return false;
	double col[2][3];
	for (int k = 0; k < 2; ++k) // images of world X and Y
		for (int r = 0; r < 3; ++r)
			col[k][r] = (cvw[r][0] * inv[0][k] + cvw[r][1] * inv[1][k] + cvw[r][2] * inv[2][k]) / det;
	auto len = [](const double *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
	const double lx = len(col[0]), ly0 = len(col[1]);
	if (lx < 0.9 || lx > 1.1 || ly0 < 0.9 || ly0 > 1.1)
		return false; // not a rigid view transform (bad samples)
	double x[3], y[3], z[3];
	for (int r = 0; r < 3; ++r) x[r] = col[0][r] / lx;
	const double dxy = x[0] * col[1][0] + x[1] * col[1][1] + x[2] * col[1][2];
	for (int r = 0; r < 3; ++r) y[r] = col[1][r] - dxy * x[r];
	const double ly = len(y);
	for (int r = 0; r < 3; ++r) y[r] /= ly;
	z[0] = x[1] * y[2] - x[2] * y[1]; z[1] = x[2] * y[0] - x[0] * y[2]; z[2] = x[0] * y[1] - x[1] * y[0];
	const double mv[3] = {a.sv[0] * inv_n, a.sv[1] * inv_n, a.sv[2] * inv_n};
	const double *axes[3] = {x, y, z};
	for (int k = 0; k < 3; ++k) {
		s_fog_rx[k] = float(x[k]); s_fog_ry[k] = float(y[k]); s_fog_rz[k] = float(z[k]);
		const double *ax = axes[k];
		s_fog_cam[k] = float(a.wref[k] + a.sw[k] * inv_n - (ax[0] * mv[0] + ax[1] * mv[1] + ax[2] * mv[2]));
	}
	return true;
}

// from on_draw: the game's target, viewport and dxwrapper constants are live during the first captured draw
static void fog_on_draw()
{
	if (!s_mir_n || !fx_depth_wanted())
		return;
	if (!s_fog_rt) {
		s_dev->GetViewport(&s_fog_vp);
		s_dev->GetRenderTarget(0, &s_fog_rt);
		s_dev->GetVertexShaderConstantF(0, s_fog_dxw, 2);
	}
	if (s_current_poly && !s_fog_gf_done) { // GrayFace's buffer mirrors the terrain TL array during this draw
		s_fog_gf_done = true;
		update_grayface_scale(s_current_n);
	}
}

static void fog_release_captured()
{
	s_fog_tris.clear();
	s_fog_terrain = false;
	s_fog_gf_done = false;
	s_fog_acc = FogAcc();
	if (s_fog_rt) {
		s_fog_rt->Release();
		s_fog_rt = nullptr;
	}
}

// before the captures are released (projection fit, sky flag); also gives bb_on_draw this frame's projection
static bool fog_prepare()
{
	if (!fx_depth_wanted() || !s_dev || !s_fog_terrain || !s_fog_rt || s_fog_tris.empty() || !s_fog_vp.Width || !s_fog_vp.Height)
		return false;
	float ax, bx, ay, by;
	if (!s_mir_fit_x.solve(ax, bx) || !s_mir_fit_y.solve(ay, by) || !(bx * by > 0) || bx / by < 0.5f || bx / by > 2.0f)
		return false;
	s_fog_proj[0] = ax; s_fog_proj[1] = bx; s_fog_proj[2] = ay; s_fog_proj[3] = by;
	std::memcpy(s_mir_last_fit, s_fog_proj, sizeof(s_fog_proj));
	s_mir_last_fit_ok = true;
	if (fog_solve_basis())
		s_fog_basis_ok = true; // otherwise keep the previous frame's camera
	s_fog_sky_ok = s_sky_ready && s_sky_tex;
	return s_fog_basis_ok;
}

static const char s_fog_src[] = R"(
sampler2D tex0 : register(s0);     // depth pass: billboard texture; fog pass: forward depth (0 = sky)
sampler2D skyTex : register(s1);   // frame after the sky pass
sampler2D noiseTex : register(s2); // value noise, 4 independent channels
float4 proj : register(c0);  // game px = proj.x + proj.y * y / x, py = proj.z + proj.w * z / x (view: x fwd, z up)
float4 gf : register(c1);    // GrayFace HD scaling: xy * gf.xy + gf.zw
float4 off : register(c2);   // dxwrapper fix-up offset
float4 mul : register(c3);   // dxwrapper fix-up multiplier
float4 zr : register(c4);    // x = near, y = far / (far - near)
float4 rt : register(c5);    // 1/RT width, 1/RT height, sky texture valid, -
float4 vp : register(c6);    // viewport X, Y, W, H
float4 rxc : register(c7);   // xyz: view image of world X, w: camera world x
float4 ryc : register(c8);
float4 rzc : register(c9);
float4 sun : register(c10);  // xyz: world sun direction, w: sun visibility
float4 pal : register(c11);  // rgb: time-of-day fog colour, w: water world z
float4 sunc : register(c12); // rgb: sun colour, w: draw distance (forward depth)
float4 dens : register(c13); // density, 1/H, max distance, noise scale
float4 drift : register(c14);// wind offsets (world units) of the coarse (xy) and fine (zw) octave
float4 view : register(c15); // game 3D view rect: left, top, right, bottom
float4 look : register(c16); // HG g, sun glow, sky haze elevation, edge start (fraction of draw distance)
float4 ramp : register(c17); // fog start, 1 / fog ramp
float4 atest : register(c18);// x = 1: alpha-test tex0 (billboards)

struct DOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float depth : TEXCOORD1; };
DOut vs_depth(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	DOut o;
	float w = pos.x;
	float2 gw = float2(proj.x * w + proj.y * pos.y, proj.z * w + proj.w * pos.z); // game px * w
	o.pos = float4((gw * gf.xy + (gf.zw + off.xy) * w) * mul.xy, (w - zr.x) * zr.y, w);
	o.uv = uv;
	o.depth = w;
	return o;
}
float4 ps_depth(float2 uv : TEXCOORD0, float depth : TEXCOORD1) : COLOR
{
	float a = tex2D(tex0, uv).a;
	clip(atest.x > 0.5 ? a - 0.5 : 1.0);
	return float4(depth, 0.0, 0.0, 1.0);
}
float4 vs_quad(float3 pos : POSITION) : POSITION // pos.xy in game screen coords
{
	return float4((pos.xy * gf.xy + gf.zw + off.xy) * mul.xy, 0.0, 1.0);
}

float2 to_game(float2 px)
{
	float2 ndc = float2((px.x - vp.x) / vp.z * 2.0 - 1.0, 1.0 - (px.y - vp.y) / vp.w * 2.0);
	return ((ndc / mul.xy - off.xy) - gf.zw) / gf.xy;
}
float2 to_rt_uv(float2 g)
{
	float2 ndc = (g * gf.xy + gf.zw + off.xy) * mul.xy;
	return float2(vp.x + (ndc.x + 1.0) * 0.5 * vp.z, vp.y + (1.0 - ndc.y) * 0.5 * vp.w) * rt.xy;
}
// sky colour a little above the horizon in game column x; w = 1 if that point is inside the 3D view
float4 sky_near_horizon(float x)
{
	float ty = (x - proj.x) / proj.y;
	float tz = -(rzc.x + rzc.y * ty) / (abs(rzc.z) > 0.05 ? rzc.z : 0.05) + 0.06;
	float y = proj.z + proj.w * tz;
	float ok = step(view.y + 2.0, y) * step(y, view.w - 2.0) * step(0.05, abs(rzc.z));
	return float4(tex2Dlod(skyTex, float4(to_rt_uv(float2(x, clamp(y, view.y, view.w))), 0.0, 0.0)).rgb * ok, ok);
}

static const int Steps = 12;
float4 ps_fog(float2 vpos : VPOS) : COLOR
{
	float2 px = vpos + 0.5;
	float depth = tex2Dlod(tex0, float4(px * rt.xy, 0.0, 0.0)).r;
	float2 gp = to_game(px);
	float3 dv = float3(1.0, (gp.x - proj.x) / proj.y, (gp.y - proj.z) / proj.w); // view ray with forward = 1
	float dl = length(dv);
	float3 wd = float3(dot(rxc.xyz, dv), dot(ryc.xyz, dv), dot(rzc.xyz, dv)) / dl;
	float3 cam = float3(rxc.w, ryc.w, rzc.w);
	bool sky = depth <= 0.0;
	float len = sky ? dens.z : min(depth * dl, dens.z);
	float jit = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715)))); // interleaved gradient noise
	float stp = len / Steps;
	float tau = 0.0;
	[loop] for (int i = 0; i < Steps; ++i) {
		float s = (i + jit) * stp;
		float3 p = cam + wd * s;
		float h = max(p.z - pal.w, 0.0);
		float2 q = p.xy + p.z * float2(0.6, 0.35); // height shears the 2D noise: blobs change with altitude
		float n = tex2Dlod(noiseTex, float4((q + drift.xy) * dens.w, 0.0, 0.0)).r * 0.65
		        + tex2Dlod(noiseTex, float4((q + drift.zw) * dens.w * 3.0, 0.0, 0.0)).g * 0.35;
		// banks with gaps hugging water and hollows + a thin haze higher up
		float bank = pow(saturate(n * 1.7 - 0.45), 1.4) * 2.4;
		float dn = exp(-h * dens.y) * bank + 0.22 * exp(-h * (1.0 / 2500.0));
		tau += dn * saturate((s - ramp.x) * ramp.y);
	}
	float T = exp(-tau * dens.x * stp);
	if (sky)
		T *= smoothstep(-0.02, look.z, wd.z);                     // horizon haze: the horizon line dissolves
	else
		T *= 1.0 - smoothstep(look.w * sunc.w, sunc.w, depth);    // draw-distance edge disappears in the fog
	// fog colour: time-of-day palette pulled toward the actual sky near the horizon
	float3 fc = pal.rgb;
	if (rt.z > 0.5) {
		float4 s = sky_near_horizon(lerp(view.x, view.z, 0.2)) + sky_near_horizon(lerp(view.x, view.z, 0.5))
		         + sky_near_horizon(lerp(view.x, view.z, 0.8));
		if (s.w > 0.0)
			fc = lerp(fc, s.rgb / s.w, 0.7 * s.w / 3.0);
	}
	float g = look.x;
	float hg = pow((1.0 - g) * (1.0 - g) / (1.0 + g * g - 2.0 * g * dot(wd, sun.xyz)), 1.5); // HG, 1 toward the sun
	float3 L = fc + sunc.rgb * (look.y * hg * sun.w);
	return float4(L * (1.0 - T), T); // blend ONE, SRCALPHA: dst * T + L * (1 - T)
}
)";

static bool init_fog_resources()
{
	if (s_fog_ps || s_fog_init_failed)
		return s_fog_ps != nullptr;
	s_fog_init_failed = true;
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	auto compile = dc ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dc, "D3DCompile")) : nullptr;
	if (!compile)
		return false;
	ID3DBlob *blobs[4] = {};
	const char *const entries[4][2] = {{"vs_depth", "vs_3_0"}, {"ps_depth", "ps_3_0"}, {"vs_quad", "vs_3_0"}, {"ps_fog", "ps_3_0"}};
	HRESULT hr = S_OK;
	for (int i = 0; i < 4 && SUCCEEDED(hr); ++i) {
		ID3DBlob *err = nullptr;
		hr = compile(s_fog_src, sizeof(s_fog_src) - 1, "mm7fx_fog", nullptr, nullptr, entries[i][0], entries[i][1], 0, 0, &blobs[i], &err);
		if (FAILED(hr))
			log_info("mm7fx: fog %s compile failed 0x%08lx: %s", entries[i][0], hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
		if (err) err->Release();
	}
	auto code = [&](int i) { return static_cast<const DWORD *>(blobs[i]->GetBufferPointer()); };
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexShader(code(0), &s_fog_depth_vs);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(code(1), &s_fog_depth_ps);
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexShader(code(2), &s_fog_quad_vs);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(code(3), &s_fog_ps);
	for (ID3DBlob *b : blobs)
		if (b) b->Release();
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexDeclaration(s_mirror_decl_elems, &s_fog_decl);
	if (SUCCEEDED(hr)) hr = s_dev->CreateStateBlock(D3DSBT_ALL, &s_fog_state);
	// noise: random bytes, bilinear filtering makes it value noise; uploaded via a system-memory copy (works on 9Ex too)
	IDirect3DTexture9 *sys = nullptr;
	if (SUCCEEDED(hr)) hr = s_dev->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
	if (SUCCEEDED(hr)) hr = s_dev->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_fog_noise, nullptr);
	D3DLOCKED_RECT lr;
	if (SUCCEEDED(hr)) hr = sys->LockRect(0, &lr, nullptr, 0);
	if (SUCCEEDED(hr)) {
		uint32_t seed = 0x2545F491;
		for (int y = 0; y < 128; ++y)
			for (int x = 0; x < 128; ++x) {
				seed = seed * 1664525u + 1013904223u;
				reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + y * lr.Pitch)[x] = seed ^ (seed >> 13);
			}
		sys->UnlockRect(0);
		hr = s_dev->UpdateTexture(sys, s_fog_noise);
	}
	if (sys) sys->Release();
	if (FAILED(hr)) {
		log_info("mm7fx: fog init failed 0x%08lx, fog disabled", hr);
		return false;
	}
	s_fog_init_failed = false;
	log_info("mm7fx: fog pass ready");
	return true;
}

static void fog_release_resources()
{
	fog_release_captured();
	for (IUnknown **p : {reinterpret_cast<IUnknown **>(&s_fog_depth_vs), reinterpret_cast<IUnknown **>(&s_fog_quad_vs),
	                     reinterpret_cast<IUnknown **>(&s_fog_depth_ps), reinterpret_cast<IUnknown **>(&s_fog_ps),
	                     reinterpret_cast<IUnknown **>(&s_fog_decl), reinterpret_cast<IUnknown **>(&s_fog_state),
	                     reinterpret_cast<IUnknown **>(&s_fog_noise), reinterpret_cast<IUnknown **>(&s_fog_depth_tex),
	                     reinterpret_cast<IUnknown **>(&s_fog_ds)}) {
		if (*p) (*p)->Release();
		*p = nullptr;
	}
}

static float smooth01(float e0, float e1, float x)
{
	const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

// After the game's EndScene (billboards drawn, UI not yet): depth pass + fog pass in our own Begin/EndScene.
static void fog_render()
{
	if (!init_fog_resources())
		return;
	D3DSURFACE_DESC rd;
	s_fog_rt->GetDesc(&rd);
	D3DSURFACE_DESC dd = {};
	if (s_fog_depth_tex)
		s_fog_depth_tex->GetLevelDesc(0, &dd);
	if (!s_fog_depth_tex || dd.Width != rd.Width || dd.Height != rd.Height) {
		if (s_fog_depth_tex) s_fog_depth_tex->Release();
		if (s_fog_ds) s_fog_ds->Release();
		s_fog_depth_tex = nullptr;
		s_fog_ds = nullptr;
		HRESULT hr = s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &s_fog_depth_tex, nullptr);
		if (FAILED(hr))
			hr = s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &s_fog_depth_tex, nullptr);
		if (SUCCEEDED(hr))
			hr = s_dev->CreateDepthStencilSurface(rd.Width, rd.Height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &s_fog_ds, nullptr);
		if (FAILED(hr)) {
			log_info("mm7fx: fog depth target %ux%u failed 0x%08lx, fog disabled", rd.Width, rd.Height, hr);
			if (s_fog_depth_tex) s_fog_depth_tex->Release();
			s_fog_depth_tex = nullptr;
			s_fog_init_failed = true;
			return;
		}
		log_info("mm7fx: fog depth target %ux%u", rd.Width, rd.Height);
	}

	// time of day: sun rises in the east (+X) at 6:00, culminates in the south (-Y), sets in the west at 18:00
	const int hour = std::clamp(*reinterpret_cast<const int32_t *>(mm7::GameHour), 0, 23);
	const int minute = std::clamp(*reinterpret_cast<const int32_t *>(mm7::GameMinute), 0, 59);
	const float ang = (hour + minute / 60.0f - 6.0f) / 12.0f * 3.14159265f;
	const float sun[3] = {std::cos(ang), -std::sin(ang) * std::cos(fogcfg::SunNoonElev), std::sin(ang) * std::sin(fogcfg::SunNoonElev)};
	const float day[3] = {0.70f, 0.76f, 0.84f}, dusk[3] = {0.78f, 0.58f, 0.46f}, night[3] = {0.045f, 0.06f, 0.11f};
	const float low_sun[3] = {1.0f, 0.5f, 0.22f}, high_sun[3] = {1.0f, 0.92f, 0.78f};
	float pal[3], sunc[3];
	for (int k = 0; k < 3; ++k) {
		pal[k] = sun[2] >= 0 ? dusk[k] + (day[k] - dusk[k]) * smooth01(0, 0.3f, sun[2])
		                     : dusk[k] + (night[k] - dusk[k]) * smooth01(0, 0.18f, -sun[2]);
		sunc[k] = low_sun[k] + (high_sun[k] - low_sun[k]) * smooth01(0, 0.4f, sun[2]);
	}
	// wind offsets wrapped to the noise period (1 / NoiseScale world units) so they never jump
	const double t = GetTickCount() * 0.001, period = 1.0 / fogcfg::NoiseScale;
	auto wrap = [&](double v) { return float(std::fmod(v, period)); };
	const float far_d = s_fog_far > 1000 ? s_fog_far : 12000.0f;
	const float zn = 16.0f, zf = 65536.0f;
	const float vc[20] = {s_fog_proj[0], s_fog_proj[1], s_fog_proj[2], s_fog_proj[3],
	                      s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1],
	                      s_fog_dxw[0], s_fog_dxw[1], s_fog_dxw[2], s_fog_dxw[3],
	                      s_fog_dxw[4], s_fog_dxw[5], s_fog_dxw[6], s_fog_dxw[7], zn, zf / (zf - zn), 0, 0};
	const int32_t *rr = reinterpret_cast<const int32_t *>(mm7::RenderRect);
	const float pc[14 * 4] = {
		1.0f / rd.Width, 1.0f / rd.Height, s_fog_sky_ok ? 1.0f : 0.0f, 0,
		float(s_fog_vp.X), float(s_fog_vp.Y), float(s_fog_vp.Width), float(s_fog_vp.Height),
		s_fog_rx[0], s_fog_rx[1], s_fog_rx[2], s_fog_cam[0],
		s_fog_ry[0], s_fog_ry[1], s_fog_ry[2], s_fog_cam[1],
		s_fog_rz[0], s_fog_rz[1], s_fog_rz[2], s_fog_cam[2],
		sun[0], sun[1], sun[2], smooth01(-0.06f, 0.04f, sun[2]),
		pal[0], pal[1], pal[2], s_fog_base_z,
		sunc[0], sunc[1], sunc[2], far_d,
		fogcfg::Density, 1.0f / fogcfg::HeightFalloff, fogcfg::MaxDist, fogcfg::NoiseScale,
		wrap(fogcfg::WindX * t), wrap(fogcfg::WindY * t), wrap(fogcfg::WindX * 1.6 * t), wrap(fogcfg::WindY * 1.6 * t),
		float(rr[0]), float(rr[1]), float(rr[2]), float(rr[3]),
		fogcfg::SunG, fogcfg::SunGlow, fogcfg::SkyHazeElev, fogcfg::EdgeStart,
		fogcfg::FogStart, 1.0f / fogcfg::FogRamp, 0, 0,
		0, 0, 0, 0}; // atest

	const bool began = SUCCEEDED(s_dev->BeginScene());
	s_fog_state->Capture();
	IDirect3DSurface9 *prev_rt = nullptr, *prev_ds = nullptr, *depth_rt = nullptr;
	s_dev->GetRenderTarget(0, &prev_rt);
	s_dev->GetDepthStencilSurface(&prev_ds);
	s_fog_depth_tex->GetSurfaceLevel(0, &depth_rt);

	// 1. linear depth
	s_dev->SetRenderTarget(0, depth_rt);
	s_dev->SetDepthStencilSurface(s_fog_ds);
	s_dev->SetViewport(&s_fog_vp);
	s_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
	s_dev->SetVertexShaderConstantF(0, vc, 5);
	s_dev->SetPixelShaderConstantF(0, vc, 5);
	s_dev->SetPixelShaderConstantF(5, pc, 14);
	s_dev->SetVertexDeclaration(s_fog_decl);
	s_dev->SetVertexShader(s_fog_depth_vs);
	s_dev->SetPixelShader(s_fog_depth_ps);
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	s_dev->SetTexture(0, nullptr);
	constexpr size_t chunk = 3 * 20000;
	for (size_t i = 0; i < s_fog_tris.size(); i += chunk)
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(std::min(chunk, s_fog_tris.size() - i) / 3), &s_fog_tris[i], sizeof(MirrorVertex));
	const float at[4] = {1, 0, 0, 0};
	s_dev->SetPixelShaderConstantF(18, at, 1);
	for (DWORD s = 0; s < 3; ++s) {
		s_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		s_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, s == 2 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, s == 2 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		s_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		s_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_NONE); // billboard alpha: mips
	}
	for (const auto &p : s_bb_polys) { // ponytail: one draw per billboard; batch by texture like the mirror if it shows up in profiles
		s_dev->SetTexture(0, p.tex);
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_bb_verts[p.first], sizeof(MirrorVertex));
	}

	shadows_render(sun); // sun shadows + AO under the fog
	// 2. fog over the game's 3D view
	if (fog_wanted() && !shadows_debug()) {
	s_dev->SetRenderTarget(0, s_fog_rt);
	s_dev->SetDepthStencilSurface(nullptr);
	s_dev->SetViewport(&s_fog_vp);
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	s_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
	s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
	s_dev->SetVertexShader(s_fog_quad_vs);
	s_dev->SetPixelShader(s_fog_ps);
	s_dev->SetTexture(0, s_fog_depth_tex);
	s_dev->SetTexture(1, s_fog_sky_ok ? s_sky_tex : nullptr);
	s_dev->SetTexture(2, s_fog_noise);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); // depth: never filter across silhouettes
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7); // leave the target's alpha alone
	const float l = float(rr[0]), tp = float(rr[1]), r = float(rr[2]), b = float(rr[3]);
	const MirrorVertex quad[4] = {{l, tp, 0, 0, 0, 0}, {r, tp, 0, 0, 0, 0}, {r, b, 0, 0, 0, 0}, {l, b, 0, 0, 0, 0}};
	s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(MirrorVertex));
	}

	if (prev_rt) s_dev->SetRenderTarget(0, prev_rt);
	s_dev->SetDepthStencilSurface(prev_ds);
	s_fog_state->Apply(); // after the targets: SetRenderTarget resets the viewport
	if (prev_rt) prev_rt->Release();
	if (prev_ds) prev_ds->Release();
	depth_rt->Release();
	if (began)
		s_dev->EndScene();
	static DWORD last = 0;
	if (GetTickCount() - last > 5000) {
		last = GetTickCount();
		log_info("mm7fx dbg: fog %zu tris %zu billboards, cam (%.0f %.0f %.0f) up (%.3f %.3f %.3f) water z %.0f far %.0f, %02d:%02d sun z %.2f",
			s_fog_tris.size() / 3, s_bb_polys.size(), s_fog_cam[0], s_fog_cam[1], s_fog_cam[2], s_fog_rz[0], s_fog_rz[1], s_fog_rz[2],
			s_fog_base_z, far_d, hour, minute, sun[2]);
	}
}

// ---- sun shadows + ambient occlusion ---------------------------------------------------------
// Runs inside fog_render, after its linear-depth pass and before the fog itself (also when the fog is off).
//  1. shadow map: this frame's captured geometry (s_fog_tris, view space) and billboards (alpha-tested, turned to face
//     the sun about their vertical axis and pushed away from it so they don't shadow themselves) rendered from the
//     sun with an orthographic projection over Extent x Extent world units centred Ahead of the camera, origin snapped
//     to texels; R32F holds the light-space z (smaller = nearer the sun), cleared to 1 = no caster.
//  2. half resolution: per pixel view position from the fog's linear depth, normal from neighbour depths, 4x4 tent PCF
//     with normal offset + slope-scaled bias, back faces to the sun shaded; 12-sample screen-space AO (normal-cosine
//     weighted, 2x2 rotated pattern). Output r = AO, g = sun visibility, b = depth.
//  3. full resolution: depth-aware 4x4 upsample (also removes the AO pattern), multiplied into the 3D view
//     (DESTCOLOR, ZERO): lerp(ShadowColor, 1, visibility) * AO.
// Casters are only what the game drew this frame (inside the view): off-screen buildings cast nothing.
// Toggle: Effect mode only, and mm7fx_shadows.txt (0 off, 1 on, 2 debug: the factor as grey over the 3D view, no fog).

namespace shcfg { // MM7 units: a terrain tile is 512
constexpr int MapSize = 1024;          // shadow map texels per side
constexpr float Extent = 6000;         // world units covered by the map (perpendicular to the sun)
constexpr float Ahead = 1500;          // map centre this far ahead of the camera (horizontally)
constexpr float DepthRange = 20000;    // light-space depth +- around the centre
constexpr float Strength = 1.0f;       // 1: fully shadowed pixels get ShadowColor
constexpr float ShadowColor[3] = {0.58f, 0.60f, 0.68f}; // multiplier in full shadow (ambient, slightly blue)
constexpr float FadeStart = 2200, FadeEnd = 3200; // shadows fade out with camera distance
constexpr float SunLow = 0.04f, SunHigh = 0.25f; // shadows fade in with sin(sun elevation) between these
constexpr float Bias = 6;              // world units along the sun
constexpr float SlopeBias = 1.5f;      // shadow-map texels * tan(angle to the sun), capped at 6
constexpr float NormalOffset = 4;      // receiver pushed along its normal, world units
constexpr float BackfaceShade = 0.6f;  // 0..1: surfaces facing away from the sun count as shadowed
constexpr float BillboardPush = 0.7f;  // billboard casters pushed away from the sun by this * half width
constexpr float AoRadius = 90;         // world units
constexpr float AoStrength = 0.6f;
constexpr float AoMaxPx = 40;          // screen radius cap, game px (640x480)
constexpr float AoCosBias = 0.15f;     // ignore occluders flatter than this (cosine to the normal)
constexpr float AoFadeStart = 3000, AoFadeEnd = 6000;
}

static int s_sh_mode = 1; // mm7fx_shadows.txt
static IDirect3DTexture9 *s_sh_map = nullptr, *s_sh_half = nullptr;
static IDirect3DSurface9 *s_sh_map_ds = nullptr;
static IDirect3DVertexShader9 *s_sh_vs_map = nullptr, *s_sh_vs_quad = nullptr;
static IDirect3DPixelShader9 *s_sh_ps_map = nullptr, *s_sh_ps_half = nullptr, *s_sh_ps_apply = nullptr;
static IDirect3DStateBlock9 *s_sh_state = nullptr;
static bool s_sh_init_failed = false;
static std::vector<MirrorVertex> s_sh_bb; // billboards turned to the sun

static bool shadows_wanted() { return s_mode == Mode::Effect && s_sh_mode != 0; }
static bool shadows_debug() { return shadows_wanted() && s_sh_mode == 2; }

static void shadows_poll_toggle()
{
	if (FILE *f = std::fopen("mm7fx_shadows.txt", "r")) {
		int v = 1;
		if (std::fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 2 && v != s_sh_mode) {
			s_sh_mode = v;
			log_info("mm7fx: shadows %d (from mm7fx_shadows.txt)", v);
		}
		std::fclose(f);
	}
}

static const char s_sh_src[] = R"(
sampler2D tex0 : register(s0);    // map pass: billboard texture; other passes: linear depth (0 = sky)
sampler2D smap : register(s1);    // shadow map
sampler2D halfTex : register(s2); // half res: r = AO, g = sun visibility, b = depth
float4 proj : register(c0);  // game px = proj.x + proj.y * y / x, py = proj.z + proj.w * z / x (view: x fwd, z up)
float4 gf : register(c1);    // GrayFace HD scaling
float4 off : register(c2);   // dxwrapper fix-up offset
float4 mul : register(c3);   // dxwrapper fix-up multiplier
float4 rt : register(c4);    // 1/RT width, 1/RT height, 1/half width, 1/half height
float4 vp : register(c5);    // viewport X, Y, W, H
float4 lr0 : register(c6);   // view -> light space rows (world units): x, y across the map, z toward the sun
float4 lr1 : register(c7);
float4 lr2 : register(c8);
float4 sunv : register(c9);  // xyz: view-space direction to the sun, w: shadow strength (0 at night)
float4 sm : register(c10);   // 1 / half extent, 1 / (2 * depth range), map size, texel size (world)
float4 bias : register(c11); // bias, slope bias (texels), normal offset, back-face shade
float4 fade : register(c12); // shadow fade start, 1 / length, AO fade start, 1 / length
float4 aoc : register(c13);  // AO radius, strength, max radius (game px), cosine bias
float4 col : register(c14);  // rgb: shadow colour, w: debug
float4 atest : register(c15);// x = 1: alpha-test tex0

struct MOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float z : TEXCOORD1; };
MOut vs_map(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	float3 l = float3(dot(lr0.xyz, pos) + lr0.w, dot(lr1.xyz, pos) + lr1.w, dot(lr2.xyz, pos) + lr2.w);
	MOut o;
	o.pos = float4(l.xy * sm.x, 0.5 - l.z * sm.y, 1.0);
	o.uv = uv;
	o.z = o.pos.z;
	return o;
}
float4 ps_map(float2 uv : TEXCOORD0, float z : TEXCOORD1) : COLOR
{
	clip(atest.x > 0.5 ? tex2D(tex0, uv).a - 0.5 : 1.0);
	return float4(z, 0.0, 0.0, 1.0);
}
float4 vs_quad(float3 pos : POSITION) : POSITION // pos.xy in game screen coords
{
	return float4((pos.xy * gf.xy + gf.zw + off.xy) * mul.xy, 0.0, 1.0);
}

float2 to_game(float2 px)
{
	float2 ndc = float2((px.x - vp.x) / vp.z * 2.0 - 1.0, 1.0 - (px.y - vp.y) / vp.w * 2.0);
	return ((ndc / mul.xy - off.xy) - gf.zw) / gf.xy;
}
float2 to_rt_uv(float2 g)
{
	float2 ndc = (g * gf.xy + gf.zw + off.xy) * mul.xy;
	return float2(vp.x + (ndc.x + 1.0) * 0.5 * vp.z, vp.y + (1.0 - ndc.y) * 0.5 * vp.w) * rt.xy;
}
float3 view_dir(float2 g) { return float3(1.0, (g.x - proj.x) / proj.y, (g.y - proj.z) / proj.w); }
float depth_at(float2 px) { return tex2Dlod(tex0, float4(px * rt.xy, 0.0, 0.0)).r; }
float3 view_at(float2 px, float d) { return d * view_dir(to_game(px)); }

float4 ps_half(float2 vpos : VPOS) : COLOR
{
	float2 px = vpos * 2.0 + 0.5; // full-res pixel 2 * vpos
	float d = depth_at(px);
	if (d <= 0.0)
		return float4(1.0, 1.0, 0.0, 1.0);
	float3 P = view_at(px, d);
	// normal from the neighbour on the side with the smaller depth step (no smearing across silhouettes)
	float dl = depth_at(px - float2(1.0, 0.0)), dr = depth_at(px + float2(1.0, 0.0));
	float du = depth_at(px - float2(0.0, 1.0)), dd = depth_at(px + float2(0.0, 1.0));
	float3 ex = abs(dr - d) < abs(d - dl) ? view_at(px + float2(1.0, 0.0), dr) - P : P - view_at(px - float2(1.0, 0.0), dl);
	float3 ey = abs(dd - d) < abs(d - du) ? view_at(px + float2(0.0, 1.0), dd) - P : P - view_at(px - float2(0.0, 1.0), du);
	float3 N = normalize(cross(ex, ey));
	N = dot(N, P) > 0.0 ? -N : N;
	float dist = length(P);

	// --- sun shadow ---
	float vis = 1.0;
	float sf = sunv.w * (1.0 - saturate((dist - fade.x) * fade.y));
	if (sf > 0.001) {
		float ndl = dot(N, sunv.xyz);
		float3 Q = P + N * bias.z;
		float3 l = float3(dot(lr0.xyz, Q) + lr0.w, dot(lr1.xyz, Q) + lr1.w, dot(lr2.xyz, Q) + lr2.w);
		float2 uv = float2(l.x * sm.x * 0.5 + 0.5, 0.5 - l.y * sm.x * 0.5) + 0.5 / sm.z; // D3D9 half-texel
		sf *= 1.0 - smoothstep(0.85, 1.0, max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0);
		float tanl = sqrt(saturate(1.0 - ndl * ndl)) / max(abs(ndl), 0.05);
		float rz = 0.5 - l.z * sm.y - (bias.x + bias.y * sm.w * min(tanl, 6.0)) * sm.y;
		// 4x4 texels, tent weights = 3x3 box PCF convolved with bilinear
		float2 tc = uv * sm.z - 0.5;
		float2 b0 = floor(tc), f = tc - b0;
		float acc = 0.0;
		[unroll] for (int j = -1; j <= 2; ++j) {
			float wy = j == -1 ? 1.0 - f.y : (j == 2 ? f.y : 1.0);
			[unroll] for (int i = -1; i <= 2; ++i) {
				float wx = i == -1 ? 1.0 - f.x : (i == 2 ? f.x : 1.0);
				float s = tex2Dlod(smap, float4((b0 + float2(i, j) + 0.5) / sm.z, 0.0, 0.0)).r;
				acc += wx * wy * (s >= rz ? 1.0 : 0.0);
			}
		}
		float lit = min(acc / 9.0, lerp(1.0, smoothstep(0.0, 0.2, ndl), bias.w));
		vis = 1.0 - (1.0 - lit) * sf;
	}

	// --- ambient occlusion: screen-space disk, occluders weighted by the cosine to the normal ---
	float ao = 1.0;
	float af = 1.0 - saturate((dist - fade.z) * fade.w);
	if (af > 0.001) {
		float2 gp = to_game(px);
		float rg = min(aoc.x * abs(proj.y) / d, aoc.z);
		float2 cell = fmod(vpos, 2.0);
		float k0 = cell.x * 2.0 + cell.y;
		float occ = 0.0;
		[unroll] for (int k = 0; k < 12; ++k) {
			float a = k * 2.39996 + k0 * 1.5708 + 0.4;
			float r = sqrt((k + 0.25 + 0.5 * frac(k0 * 0.25 + 0.125)) / 12.0);
			float2 g = gp + float2(cos(a), sin(a)) * (r * rg);
			float sd = tex2Dlod(tex0, float4(to_rt_uv(g), 0.0, 0.0)).r;
			float3 v = sd * view_dir(g) - P;
			float vv = dot(v, v);
			float o = saturate(dot(v, N) * rsqrt(vv + 1e-4) - aoc.w) * saturate(1.0 - vv / (4.0 * aoc.x * aoc.x));
			occ += sd > 0.0 ? o : 0.0;
		}
		ao = 1.0 - aoc.y * af * saturate(occ / 12.0 * 2.0);
	}
	return float4(ao, vis, d, 1.0);
}

float4 ps_apply(float2 vpos : VPOS) : COLOR
{
	float2 px = vpos + 0.5;
	float d = depth_at(px);
	if (d <= 0.0)
		return float4(1.0, 1.0, 1.0, 1.0);
	// half-res texel h holds full-res pixel 2h: depth-aware tent over 4x4 texels
	float2 hc = (px - 0.5) * 0.5;
	float2 b0 = floor(hc), f = hc - b0;
	float3 acc = 0.0;
	[unroll] for (int j = -1; j <= 2; ++j) {
		float wy = j == -1 ? 1.0 - f.y : (j == 2 ? f.y : 1.0);
		[unroll] for (int i = -1; i <= 2; ++i) {
			float wx = i == -1 ? 1.0 - f.x : (i == 2 ? f.x : 1.0);
			float3 s = tex2Dlod(halfTex, float4((b0 + float2(i, j) + 0.5) * rt.zw, 0.0, 0.0)).rgb;
			float rel = abs(s.b - d) / d * 50.0;
			float w = (wx * wy + 1e-3) / (1.0 + rel * rel);
			acc += float3(s.rg, 1.0) * w;
		}
	}
	float2 av = acc.z > 1e-3 ? acc.xy / acc.z : float2(1.0, 1.0);
	float3 c = lerp(col.rgb, 1.0, av.y) * av.x;
	return float4(col.w > 0.5 ? dot(c, float3(0.299, 0.587, 0.114)).xxx : c, 1.0);
}
)";

static bool init_shadow_resources()
{
	if (s_sh_ps_apply || s_sh_init_failed)
		return s_sh_ps_apply != nullptr;
	s_sh_init_failed = true;
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	auto compile = dc ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dc, "D3DCompile")) : nullptr;
	if (!compile)
		return false;
	ID3DBlob *blobs[5] = {};
	const char *const entries[5][2] = {{"vs_map", "vs_3_0"}, {"ps_map", "ps_3_0"}, {"vs_quad", "vs_3_0"}, {"ps_half", "ps_3_0"}, {"ps_apply", "ps_3_0"}};
	HRESULT hr = S_OK;
	for (int i = 0; i < 5 && SUCCEEDED(hr); ++i) {
		ID3DBlob *err = nullptr;
		hr = compile(s_sh_src, sizeof(s_sh_src) - 1, "mm7fx_shadows", nullptr, nullptr, entries[i][0], entries[i][1], 0, 0, &blobs[i], &err);
		if (FAILED(hr))
			log_info("mm7fx: shadows %s compile failed 0x%08lx: %s", entries[i][0], hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
		if (err) err->Release();
	}
	auto code = [&](int i) { return static_cast<const DWORD *>(blobs[i]->GetBufferPointer()); };
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexShader(code(0), &s_sh_vs_map);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(code(1), &s_sh_ps_map);
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexShader(code(2), &s_sh_vs_quad);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(code(3), &s_sh_ps_half);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(code(4), &s_sh_ps_apply);
	for (ID3DBlob *b : blobs)
		if (b) b->Release();
	if (SUCCEEDED(hr)) hr = s_dev->CreateStateBlock(D3DSBT_ALL, &s_sh_state);
	if (SUCCEEDED(hr)) hr = s_dev->CreateTexture(shcfg::MapSize, shcfg::MapSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &s_sh_map, nullptr);
	if (SUCCEEDED(hr)) hr = s_dev->CreateDepthStencilSurface(shcfg::MapSize, shcfg::MapSize, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &s_sh_map_ds, nullptr);
	if (FAILED(hr)) {
		log_info("mm7fx: shadows init failed 0x%08lx, shadows disabled", hr);
		if (s_sh_ps_apply) s_sh_ps_apply->Release();
		s_sh_ps_apply = nullptr; // the readiness flag
		return false;
	}
	s_sh_init_failed = false;
	log_info("mm7fx: shadows ready (map %d)", shcfg::MapSize);
	return true;
}

static void shadows_release_resources()
{
	for (IUnknown **p : {reinterpret_cast<IUnknown **>(&s_sh_map), reinterpret_cast<IUnknown **>(&s_sh_half),
	                     reinterpret_cast<IUnknown **>(&s_sh_map_ds), reinterpret_cast<IUnknown **>(&s_sh_vs_map),
	                     reinterpret_cast<IUnknown **>(&s_sh_vs_quad), reinterpret_cast<IUnknown **>(&s_sh_ps_map),
	                     reinterpret_cast<IUnknown **>(&s_sh_ps_half), reinterpret_cast<IUnknown **>(&s_sh_ps_apply),
	                     reinterpret_cast<IUnknown **>(&s_sh_state)}) {
		if (*p) (*p)->Release();
		*p = nullptr;
	}
}

// From fog_render, inside its BeginScene, with s_fog_depth_tex holding this frame's linear depth.
// sun: world direction to the sun (fog's time-of-day model). Restores targets and state.
static void shadows_render(const float sun[3])
{
	if (!shadows_wanted() || !s_fog_depth_tex || !s_fog_decl || !init_shadow_resources())
		return;
	D3DSURFACE_DESC rd, hd = {};
	s_fog_rt->GetDesc(&rd);
	const UINT hw = (rd.Width + 1) / 2, hh = (rd.Height + 1) / 2;
	if (s_sh_half)
		s_sh_half->GetLevelDesc(0, &hd);
	if (!s_sh_half || hd.Width != hw || hd.Height != hh) {
		if (s_sh_half) s_sh_half->Release();
		s_sh_half = nullptr;
		if (FAILED(s_dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &s_sh_half, nullptr))) {
			log_info("mm7fx: shadows half-res target %ux%u failed, shadows disabled", hw, hh);
			s_sh_init_failed = true;
			shadows_release_resources();
			return;
		}
	}

	// light basis in world space: ls toward the sun, lx horizontal, ly = ls x lx
	const double ls[3] = {sun[0], sun[1], sun[2]};
	const double hl = std::sqrt(ls[0] * ls[0] + ls[1] * ls[1]);
	const double lx[3] = {hl > 1e-6 ? -ls[1] / hl : 1, hl > 1e-6 ? ls[0] / hl : 0, 0};
	const double ly[3] = {ls[1] * lx[2] - ls[2] * lx[1], ls[2] * lx[0] - ls[0] * lx[2], ls[0] * lx[1] - ls[1] * lx[0]};
	const float *r[3] = {s_fog_rx, s_fog_ry, s_fog_rz}; // view images of world X, Y, Z: world_k = cam_k + dot(r[k], view)
	// map centre ahead of the camera, snapped to whole texels across the map (no shimmering while moving)
	double fw[2] = {r[0][0], r[1][0]};
	const double fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1]);
	const double ahead = fl > 1e-3 ? shcfg::Ahead / fl : 0;
	const double c[3] = {s_fog_cam[0] + fw[0] * ahead, s_fog_cam[1] + fw[1] * ahead, s_fog_cam[2]};
	const double texel = double(shcfg::Extent) / shcfg::MapSize;
	auto dot3 = [](const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	const double cam[3] = {s_fog_cam[0], s_fog_cam[1], s_fog_cam[2]};
	const double origin[3] = {std::floor(dot3(lx, c) / texel) * texel, std::floor(dot3(ly, c) / texel) * texel, dot3(ls, c)};
	float rows[12];
	const double *axes[3] = {lx, ly, ls};
	for (int a = 0; a < 3; ++a) {
		for (int k = 0; k < 3; ++k)
			rows[a * 4 + k] = float(axes[a][0] * r[0][k] + axes[a][1] * r[1][k] + axes[a][2] * r[2][k]);
		rows[a * 4 + 3] = float(dot3(axes[a], cam) - origin[a]);
	}
	const float sunv[3] = {rows[8], rows[9], rows[10]}; // view-space direction to the sun
	const float sun_f = shcfg::Strength * smooth01(shcfg::SunLow, shcfg::SunHigh, sun[2]);

	// billboards turned to face the sun about the world vertical through their centre (a camera-facing card seen from
	// the side casts a sliver), then pushed away from the sun: same shadow on the ground, but the card's own pixels
	// (camera-facing) are no longer behind it
	s_sh_bb.clear();
	if (sun_f > 0) {
		const float *up = s_fog_rz;
		float h[3] = {-up[0] * up[1], 1 - up[1] * up[1], -up[2] * up[1]}; // camera left, horizontal
		float sh[3] = {sunv[0], sunv[1], sunv[2]};
		const float su = sh[0] * up[0] + sh[1] * up[1] + sh[2] * up[2];
		for (int k = 0; k < 3; ++k) sh[k] -= su * up[k];
		float side[3] = {up[1] * sh[2] - up[2] * sh[1], up[2] * sh[0] - up[0] * sh[2], up[0] * sh[1] - up[1] * sh[0]};
		const float hlen = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
		const float slen = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
		if (hlen > 1e-3f && slen > 1e-3f) {
			const float sg = (side[0] * h[0] + side[1] * h[1] + side[2] * h[2]) < 0 ? -1.0f : 1.0f;
			for (int k = 0; k < 3; ++k) { h[k] /= hlen; side[k] *= sg / slen; }
			s_sh_bb = s_bb_verts;
			for (const auto &p : s_bb_polys) {
				MirrorVertex *v = &s_sh_bb[p.first];
				float ctr[3] = {};
				for (uint32_t i = 0; i < p.count; ++i) { ctr[0] += v[i].x / p.count; ctr[1] += v[i].y / p.count; ctr[2] += v[i].z / p.count; }
				float st[4][2], half_w = 0;
				for (uint32_t i = 0; i < p.count; ++i) {
					const float d[3] = {v[i].x - ctr[0], v[i].y - ctr[1], v[i].z - ctr[2]};
					st[i][0] = d[0] * h[0] + d[1] * h[1] + d[2] * h[2];
					st[i][1] = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
					half_w = std::max(half_w, std::fabs(st[i][0]));
				}
				const float push = shcfg::BillboardPush * half_w;
				for (uint32_t i = 0; i < p.count; ++i) {
					v[i].x = ctr[0] + side[0] * st[i][0] + up[0] * st[i][1] - sunv[0] * push;
					v[i].y = ctr[1] + side[1] * st[i][0] + up[1] * st[i][1] - sunv[1] * push;
					v[i].z = ctr[2] + side[2] * st[i][0] + up[2] * st[i][1] - sunv[2] * push;
				}
			}
		}
	}

	const int32_t *rr = reinterpret_cast<const int32_t *>(mm7::RenderRect);
	const float k[16 * 4] = {
		s_fog_proj[0], s_fog_proj[1], s_fog_proj[2], s_fog_proj[3],
		s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1],
		s_fog_dxw[0], s_fog_dxw[1], s_fog_dxw[2], s_fog_dxw[3],
		s_fog_dxw[4], s_fog_dxw[5], s_fog_dxw[6], s_fog_dxw[7],
		1.0f / rd.Width, 1.0f / rd.Height, 1.0f / hw, 1.0f / hh,
		float(s_fog_vp.X), float(s_fog_vp.Y), float(s_fog_vp.Width), float(s_fog_vp.Height),
		rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6], rows[7], rows[8], rows[9], rows[10], rows[11],
		sunv[0], sunv[1], sunv[2], sun_f,
		2.0f / shcfg::Extent, 0.5f / shcfg::DepthRange, float(shcfg::MapSize), float(texel),
		shcfg::Bias, shcfg::SlopeBias, shcfg::NormalOffset, shcfg::BackfaceShade,
		shcfg::FadeStart, 1.0f / (shcfg::FadeEnd - shcfg::FadeStart), shcfg::AoFadeStart, 1.0f / (shcfg::AoFadeEnd - shcfg::AoFadeStart),
		shcfg::AoRadius, shcfg::AoStrength, shcfg::AoMaxPx, shcfg::AoCosBias,
		shcfg::ShadowColor[0], shcfg::ShadowColor[1], shcfg::ShadowColor[2], shadows_debug() ? 1.0f : 0.0f,
		0, 0, 0, 0}; // atest

	s_sh_state->Capture();
	IDirect3DSurface9 *prev_rt = nullptr, *prev_ds = nullptr, *surf = nullptr;
	s_dev->GetRenderTarget(0, &prev_rt);
	s_dev->GetDepthStencilSurface(&prev_ds);
	s_dev->SetVertexShaderConstantF(0, k, 16);
	s_dev->SetPixelShaderConstantF(0, k, 16);
	s_dev->SetVertexDeclaration(s_fog_decl); // MirrorVertex
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
	s_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
	for (DWORD s = 0; s < 3; ++s) {
		s_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		s_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		s_dev->SetSamplerState(s, D3DSAMP_MINFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		s_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		s_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_NONE); // billboard alpha: mips
	}

	// 1. shadow map
	s_sh_map->GetSurfaceLevel(0, &surf);
	s_dev->SetRenderTarget(0, surf);
	surf->Release();
	s_dev->SetDepthStencilSurface(s_sh_map_ds);
	const D3DVIEWPORT9 mvp = {0, 0, DWORD(shcfg::MapSize), DWORD(shcfg::MapSize), 0.0f, 1.0f};
	s_dev->SetViewport(&mvp);
	s_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0); // r = 1: no caster
	if (sun_f > 0) {
		s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		s_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		s_dev->SetVertexShader(s_sh_vs_map);
		s_dev->SetPixelShader(s_sh_ps_map);
		s_dev->SetTexture(0, nullptr);
		constexpr size_t chunk = 3 * 20000;
		for (size_t i = 0; i < s_fog_tris.size(); i += chunk)
			s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(std::min(chunk, s_fog_tris.size() - i) / 3), &s_fog_tris[i], sizeof(MirrorVertex));
		if (!s_sh_bb.empty()) {
			const float at[4] = {1, 0, 0, 0};
			s_dev->SetPixelShaderConstantF(15, at, 1);
			for (const auto &p : s_bb_polys) { // ponytail: one draw per billboard, like the fog's depth pass
				s_dev->SetTexture(0, p.tex);
				s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_sh_bb[p.first], sizeof(MirrorVertex));
			}
		}
	}

	// 2. half resolution: AO + sun visibility + depth
	s_sh_half->GetSurfaceLevel(0, &surf);
	s_dev->SetRenderTarget(0, surf);
	surf->Release();
	s_dev->SetDepthStencilSurface(nullptr);
	const D3DVIEWPORT9 hvp = {s_fog_vp.X / 2, s_fog_vp.Y / 2, std::min<DWORD>((s_fog_vp.Width + 1) / 2, hw - s_fog_vp.X / 2),
	                          std::min<DWORD>((s_fog_vp.Height + 1) / 2, hh - s_fog_vp.Y / 2), 0.0f, 1.0f};
	s_dev->SetViewport(&hvp);
	s_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00FFFF00, 1.0f, 0); // AO 1, visible, depth 0
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	s_dev->SetVertexShader(s_sh_vs_quad);
	s_dev->SetPixelShader(s_sh_ps_half);
	s_dev->SetTexture(0, s_fog_depth_tex);
	s_dev->SetTexture(1, s_sh_map);
	s_dev->SetTexture(2, nullptr);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	const float l = float(rr[0]), tp = float(rr[1]), rt = float(rr[2]), b = float(rr[3]);
	const MirrorVertex quad[4] = {{l, tp, 0, 0, 0, 0}, {rt, tp, 0, 0, 0, 0}, {rt, b, 0, 0, 0, 0}, {l, b, 0, 0, 0, 0}};
	s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(MirrorVertex));

	// 3. multiply into the game's 3D view (debug: replace)
	s_dev->SetRenderTarget(0, s_fog_rt);
	s_dev->SetViewport(&s_fog_vp);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, shadows_debug() ? FALSE : TRUE);
	s_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
	s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
	s_dev->SetPixelShader(s_sh_ps_apply);
	s_dev->SetTexture(1, nullptr);
	s_dev->SetTexture(2, s_sh_half);
	s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(MirrorVertex));

	s_dev->SetRenderTarget(0, prev_rt);
	s_dev->SetDepthStencilSurface(prev_ds);
	s_sh_state->Apply(); // after the targets: SetRenderTarget resets the viewport
	if (prev_rt) prev_rt->Release();
	if (prev_ds) prev_ds->Release();
	static DWORD last = 0;
	if (GetTickCount() - last > 5000) {
		last = GetTickCount();
		log_info("mm7fx dbg: shadows %zu tris %zu billboards, sun (%.2f %.2f %.2f) strength %.2f, centre (%.0f %.0f %.0f), mode %d",
			s_fog_tris.size() / 3, s_bb_polys.size(), sun[0], sun[1], sun[2], sun_f, c[0], c[1], c[2], s_sh_mode);
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
		const bool fog = fog_prepare(); // needs this frame's projection fit and sky flag, released below
		release_captured();
		mirror_release_captured();
		if (mirror_wanted() && s_dev) {
			bb_release(); // anything not consumed by a mirror pass
			s_bb_count = *reinterpret_cast<const uint32_t *>(mm7::BillboardCount);
			s_bb_index = 0;
			s_bb_capture = s_bb_count > 0 && s_bb_count < 2000;
		}
		s_orig_end_scene(self, edx);
		s_bb_capture = false;
		// billboards are drawn, the UI is not: fog over the finished 3D view (s_bb_* = this frame's billboards)
		if (fog)
			fog_render();
		fog_release_captured();
		return;
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
	fog_on_draw();
	if (s_bb_capture) {
		bb_on_draw();
		return false;
	}
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
		// The vanilla base pass of a water layer stays as an underlay: it fills the 1px cracks between our
		// polygons. Its lightmap/fog passes and the alpha-blended shore layer are skipped (we redraw those).
		return !(s_poly_draws == 1 && s_poly_tex);
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
		fog_poll_toggle();
		shadows_poll_toggle();
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
	fog_release_captured();
}

static void on_destroy_device(device *)
{
	bb_release();
	release_captured();
	release_mirror_resources();
	fog_release_resources();
	shadows_release_resources();
	if (s_state) s_state->Release();
	if (s_water_ps) s_water_ps->Release();
	if (s_wave_tex) s_wave_tex->Release();
	s_wave_tex = nullptr;
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
