// mm7fx: ReShade add-on for vanilla MM7 (D3D7 -> dxwrapper Dd7to9 -> D3D9 -> ReShade).
// Target: GOG mm7.exe 1.21 + GrayFace patch 2.5.7. Addresses: ../../re/water.md
//
// v2 (water proof of concept):
//   - detour Render::DrawTerrainPolygon: while it runs we know which terrain tile is being drawn;
//   - water tiles (Polygon::flags & 2): at the vanilla draw we draw the tile ourselves with our pixel shader
//     (screen-space reflection from the previous frame's 3D scene + ripples) and skip the vanilla
//     passes, keeping the game's draw order and occlusion;
//   - detour Render::DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene: copy the finished 3D scene
//     (before billboards/UI) as the reflection source for the next frame;
//   - horizon line is derived from the captured vertices (world/view/screen), no FOV guessing.
//   F10 cycles: water effect -> vanilla -> hide water -> hide all terrain.
#include <reshade.hpp>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
constexpr uintptr_t DrawBuildingsD3D = 0x478411;        // __thiscall(Render*); terrain is done when it starts
constexpr uint8_t DrawBuildingsD3DPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C}; // push ebp; mov ebp,esp; sub esp,4Ch
constexpr size_t RenderSceneDepth = 0x4010C;            // BeginScene nesting counter
constexpr uintptr_t SoftVertices = 0x50AA90;            // RenderVertexSoft[], stride 0x30
constexpr uintptr_t TLVertices = 0x77E448;              // D3DTLVERTEX[], stride 0x20, filled by DrawTerrainPolygon
constexpr size_t PolygonFlags = 0x30;                   // i16
constexpr uint16_t TileWater = 2;
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

enum class Mode { Effect, Vanilla, HideWater, HideTerrain, DebugSolid };
static Mode s_mode = Mode::Effect;
static bool s_f10_was_down = false;

static IDirect3DDevice9 *s_dev = nullptr;
static uint8_t *s_current_poly = nullptr; // set only while the game is inside DrawTerrainPolygon
static uint32_t s_current_n = 0;          // vertex count after clipping (DrawTerrainPolygon arg, not Polygon::uNumVertices)
static bool s_poly_captured = false;
static IDirect3DBaseTexture9 *s_poly_tex = nullptr; // base-pass texture of the current tile (AddRef'd)

// Raw game TL vertex (640x480 screen, z, rhw). Our vertex shader converts it to clip space like dxwrapper's
// XYZRHW fix-up shader does, so the GPU clips polygons with vertices behind/near the camera correctly.
struct WaterVertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
static const D3DVERTEXELEMENT9 s_water_decl_elems[] = {
	{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
	{0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
	{0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
	D3DDECL_END()};
struct WaterPoly { IDirect3DBaseTexture9 *tex; uint32_t first, count; };
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


	s_dev->GetRenderState(D3DRS_ZENABLE, &s_game_zenable);
	s_dev->GetRenderState(D3DRS_ZFUNC, &s_game_zfunc);
	s_dev->GetRenderState(D3DRS_ZWRITEENABLE, &s_game_zwrite);
	WaterPoly wp = {s_poly_tex, static_cast<uint32_t>(s_water_verts.size()), n}; // reference moves to the poly
	s_poly_tex = nullptr;
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
				const float dv[3] = {sv[j].vx - sv[i].vx, sv[j].vy - sv[i].vy, sv[j].vz - sv[i].vz};
				if (!gx && std::fabs(dwx) > 100 && std::fabs(dwy) < 1) { for (int k = 0; k < 3; ++k) rx[k] = dv[k] / dwx; gx = true; }
				if (!gy && std::fabs(dwy) > 100 && std::fabs(dwx) < 1) { for (int k = 0; k < 3; ++k) ry[k] = dv[k] / dwy; gy = true; }
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
	float top = vp.y * rt.y;
	float inside = saturate((ruv.y - top) * 40.0);     // reflected ray left the top of the view: no data
	ruv = float2(clamp(ruv.x, vp.x * rt.x, (vp.x + vp.z) * rt.x), clamp(ruv.y, top, (vp.y + vp.w) * rt.y));
	float3 refl = tex2D(sceneTex, ruv).rgb;
	float3 base = tex2D(waterTex, uv + rip * 4.0).rgb * color.rgb;
	float fresnel = saturate(1.0 - below * 3.5);
	float k = extra.x * (0.35 + 0.55 * fresnel) * inside;
	float3 c = lerp(base, refl * float3(0.85, 0.92, 1.0), k);
	return float4(lerp(c, float3(1.0, 0.0, 1.0), extra.w), 1.0); // extra.w = 1: debug solid magenta
}
)";

static IDirect3DPixelShader9 *s_water_ps = nullptr;
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
	ID3DBlob *vs = nullptr, *ps = nullptr;
	if (!build(s_water_vs_src, sizeof(s_water_vs_src) - 1, "vs_3_0", &vs) || !build(s_water_ps_src, sizeof(s_water_ps_src) - 1, "ps_3_0", &ps)) {
		if (vs) vs->Release();
		return false;
	}
	HRESULT hr = s_dev->CreateVertexShader(static_cast<const DWORD *>(vs->GetBufferPointer()), &s_water_vs);
	if (SUCCEEDED(hr)) hr = s_dev->CreatePixelShader(static_cast<const DWORD *>(ps->GetBufferPointer()), &s_water_ps);
	if (SUCCEEDED(hr)) hr = s_dev->CreateVertexDeclaration(s_water_decl_elems, &s_water_decl);
	if (SUCCEEDED(hr)) hr = s_dev->CreateStateBlock(D3DSBT_ALL, &s_state);
	vs->Release();
	ps->Release();
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
	for (auto &p : s_water_polys)
		if (p.tex) p.tex->Release();
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
	s_dev->SetRenderTarget(0, s_game_rt);
	s_dev->SetViewport(&vp);
	s_dev->SetVertexShader(s_water_vs);
	s_dev->SetVertexDeclaration(s_water_decl);
	s_dev->SetPixelShader(s_water_ps);
	s_dev->SetPixelShaderConstantF(0, params, 2);
	s_dev->SetPixelShaderConstantF(2, rtc, 4);
	s_dev->SetVertexShaderConstantF(0, s_dxw_off, 1);
	s_dev->SetVertexShaderConstantF(1, s_dxw_mul, 1);
	const float gfc[4] = {s_gf_mul[0], s_gf_mul[1], s_gf_add[0], s_gf_add[1]};
	s_dev->SetVertexShaderConstantF(2, gfc, 1);
	s_dev->SetPixelShaderConstantF(6, gfc, 1);
	// same depth setup the game used for its water (it may be a W-buffer)
	s_dev->SetRenderState(D3DRS_ZENABLE, s_mode == Mode::DebugSolid ? D3DZB_FALSE : s_game_zenable); // debug: no depth test
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, s_game_zwrite);
	s_dev->SetRenderState(D3DRS_ZFUNC, s_game_zfunc);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	s_dev->SetTexture(0, s_scene_tex);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	s_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DBaseTexture9 *bound = nullptr;
	for (const auto &p : s_water_polys) {
		if (p.tex != bound) {
			s_dev->SetTexture(1, p.tex);
			bound = p.tex;
		}
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_water_verts[p.first], sizeof(WaterVertex));
	}
	if (s_mode == Mode::DebugSolid) {
		// calibration quad in game screen coordinates: must cover x 100..540, y 200..400
		// calibration quad: 20%..80% of the game's view in x, 40%..80% in y (via dxwrapper's mapping)
		const float gw = 460.0f, gh = 343.0f, gx0 = 8.0f, gy0 = 8.0f; // game 3D view rect (8,8)-(468,351)
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
	s_orig_draw_terrain_polygon(self, edx, n, poly, tex, transparent, clamp);
	// TL array now holds this tile's final vertices, independent of when draw events fired.
	if (s_poly_captured && (s_mode == Mode::Effect || s_mode == Mode::DebugSolid))
		capture_water_poly();
	if (s_poly_tex) {
		s_poly_tex->Release();
		s_poly_tex = nullptr;
	}
	s_current_poly = nullptr;
}

using DrawBuildingsFn = void(__fastcall *)(void *self, void *edx);
static DrawBuildingsFn s_orig_draw_buildings = nullptr;

static void __fastcall hook_draw_buildings(void *self, void *edx)
{
	s_orig_draw_buildings(self, edx);
}

using EndSceneFn = void(__fastcall *)(void *self, void *edx);
static EndSceneFn s_orig_end_scene = nullptr;

static void __fastcall hook_end_scene(void *self, void *edx)
{
	if (*reinterpret_cast<int *>(static_cast<uint8_t *>(self) + mm7::RenderSceneDepth) == 1) {
		// Vanilla order is sky -> buildings -> terrain, so water goes after all terrain, before billboards.
		if (s_dev && (s_mode == Mode::Effect || s_mode == Mode::DebugSolid)) {
			draw_water_batch();
			// Full 3D scene: next frame's reflection source.
			s_scene_ready = copy_scene();
		}
		release_captured();
	}
	s_orig_end_scene(self, edx);
}

// ---- ReShade events --------------------------------------------------------------------------

static bool is_water(const uint8_t *poly)
{
	return *reinterpret_cast<const int16_t *>(poly + mm7::PolygonFlags) & mm7::TileWater;
}

static uint32_t s_dbg_left = 0; // diagnostic lines left for the current 5 s window

static bool on_draw(command_list *cmd, uint32_t vertex_count, uint32_t, uint32_t first_vertex, uint32_t)
{
	++s_draws;
	if (!s_dev)
		s_dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_device()->get_native()));
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
	case Mode::Effect:
		if (s_dbg_left) {
			--s_dbg_left;
			const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
			log_info("mm7fx dbg: draw vc=%u first=%u captured=%d n=%u grid=(%d,%d) tl0=(%.1f,%.1f,%.4f) tl1=(%.1f,%.1f) tl2=(%.1f,%.1f)",
				vertex_count, first_vertex, s_poly_captured, s_current_n, (int)(int8_t)s_current_poly[0x5D], (int)(int8_t)s_current_poly[0x5C],
				tl[0].x, tl[0].y, tl[0].z, tl[1].x, tl[1].y, tl[2].x, tl[2].y);
		}
		if (!s_poly_captured) {
			// inside the draw: dxwrapper's per-draw viewport, target and fix-up constants are live right now
			s_dev->GetViewport(&s_game_vp);
			if (!s_game_rt)
				s_dev->GetRenderTarget(0, &s_game_rt);
			s_dev->GetVertexShaderConstantF(0, s_dxw_off, 1);
			s_dev->GetVertexShaderConstantF(1, s_dxw_mul, 1);
			update_grayface_scale(s_current_n);
			s_dev->GetTexture(0, &s_poly_tex); // base pass texture (later passes bind lightmaps/fog)
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

	// Scriptable override: mm7fx_mode.txt in the game folder holds the mode number (0..3); checked twice a second.
	static DWORD last_poll = 0;
	if (GetTickCount() - last_poll > 500) {
		last_poll = GetTickCount();
		if (FILE *f = std::fopen("mm7fx_mode.txt", "r")) {
			int m = -1;
			if (std::fscanf(f, "%d", &m) == 1 && m >= 0 && m <= 4 && m != static_cast<int>(s_mode)) {
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
}

static void on_destroy_device(device *)
{
	release_captured();
	if (s_state) s_state->Release();
	if (s_water_ps) s_water_ps->Release();
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
