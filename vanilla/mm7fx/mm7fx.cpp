// mm7fx: ReShade add-on for vanilla MM7 (D3D7 -> dxwrapper Dd7to9 -> D3D9 -> ReShade).
// Target: GOG mm7.exe 1.21 + GrayFace patch 2.5.7. Addresses: ../../re/water.md
//
// v2 (water proof of concept):
//   - detour Render::DrawTerrainPolygon: while it runs we know which terrain tile is being drawn;
//   - water tiles (Polygon::flags & 2) are not drawn in place: their TL vertices + texture are captured;
//   - detour Render::DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene: before billboards/UI,
//     copy the frame and redraw captured water with our pixel shader (screen-space reflection + ripples);
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
constexpr size_t RenderSceneDepth = 0x4010C;            // BeginScene nesting counter
constexpr uintptr_t SoftVertices = 0x50AA90;            // RenderVertexSoft[], stride 0x30
constexpr uintptr_t TLVertices = 0x77E448;              // D3DTLVERTEX[], stride 0x20, filled by DrawTerrainPolygon
constexpr size_t PolygonFlags = 0x30;                   // i16
constexpr uint16_t TileWater = 2;

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

enum class Mode { Effect, Vanilla, HideWater, HideTerrain };
static Mode s_mode = Mode::Effect;
static bool s_f10_was_down = false;

static IDirect3DDevice9 *s_dev = nullptr;
static uint8_t *s_current_poly = nullptr; // set only while the game is inside DrawTerrainPolygon
static uint32_t s_current_n = 0;          // vertex count after clipping (DrawTerrainPolygon arg, not Polygon::uNumVertices)
static bool s_poly_captured = false;

struct WaterVertex { float x, y, z, rhw; uint32_t diffuse; float u, v, su, sv; };
constexpr DWORD WaterFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2;
struct WaterPoly { IDirect3DBaseTexture9 *tex; uint32_t first, count; };
static std::vector<WaterVertex> s_water_verts;
static std::vector<WaterPoly> s_water_polys;

// horizon fit: screen y = a + b * (v_vert / v_fwd); camera "up" from world-axis deltas of tile corners
struct Fit { double n = 0, st = 0, sy = 0, stt = 0, sty = 0; float up[3] = {}; bool have_up = false; };
static Fit s_fit;
static float s_horizon_px = -1.0f;

static uint32_t s_frame = 0, s_draws = 0, s_water_draws = 0, s_terrain_draws = 0;
static DWORD s_last_log = 0;

// ---- capture ---------------------------------------------------------------------------------

static void capture_water_poly()
{
	const uint32_t n = s_current_n;
	if (n < 3 || n > 64 || !s_dev)
		return;
	const auto *tl = reinterpret_cast<const mm7::TLVertex *>(mm7::TLVertices);
	const auto *sv = reinterpret_cast<const mm7::SoftVertex *>(mm7::SoftVertices);

	IDirect3DSurface9 *rt = nullptr;
	D3DSURFACE_DESC rd = {};
	if (SUCCEEDED(s_dev->GetRenderTarget(0, &rt))) {
		rt->GetDesc(&rd);
		rt->Release();
	}
	if (!rd.Width || !rd.Height)
		return;

	WaterPoly wp = {nullptr, static_cast<uint32_t>(s_water_verts.size()), n};
	s_dev->GetTexture(0, &wp.tex); // AddRef'd, released after drawing
	for (uint32_t i = 0; i < n; ++i) {
		const auto &t = tl[i];
		s_water_verts.push_back({t.x, t.y, t.z, t.rhw, t.diffuse, t.u, t.v, t.x / rd.Width, t.y / rd.Height});
	}
	s_water_polys.push_back(wp);

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
	s_horizon_px = static_cast<float>(a + b * (-f.up[0] / f.up[2]));
}

// ---- water pass ------------------------------------------------------------------------------

static const char s_water_ps_src[] = R"(
sampler2D sceneTex : register(s0);
sampler2D waterTex : register(s1);
float4 params : register(c0); // x = time (s), y = horizon (screen uv y), z = reflection strength
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float2 suv : TEXCOORD1) : COLOR
{
	float t = params.x;
	float2 rip = float2(sin(suv.y * 260.0 + t * 2.1) + sin(suv.x * 90.0 - t * 1.3),
	                    sin(suv.x * 170.0 + t * 1.7)) * 0.0025;
	float below = max(suv.y - params.y, 0.0);
	float2 ruv = saturate(float2(suv.x, params.y - below) + rip * (1.0 + below * 6.0));
	float3 refl = tex2D(sceneTex, ruv).rgb;
	float3 base = tex2D(waterTex, uv + rip * 4.0).rgb * color.rgb;
	float fresnel = saturate(1.0 - below * 3.5);           // grazing angles near the horizon reflect more
	float k = params.z * (0.35 + 0.55 * fresnel);
	return float4(lerp(base, refl * float3(0.85, 0.92, 1.0), k), 1.0);
}
)";

static IDirect3DPixelShader9 *s_water_ps = nullptr;
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
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = compile(s_water_ps_src, sizeof(s_water_ps_src) - 1, "mm7fx_water", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err);
	if (FAILED(hr)) {
		log_info("mm7fx: water shader compile failed 0x%08lx: %s", hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
		if (err) err->Release();
		return false;
	}
	if (err) err->Release();
	hr = s_dev->CreatePixelShader(static_cast<const DWORD *>(code->GetBufferPointer()), &s_water_ps);
	code->Release();
	if (FAILED(hr) || FAILED(s_dev->CreateStateBlock(D3DSBT_ALL, &s_state))) {
		log_info("mm7fx: CreatePixelShader/CreateStateBlock failed 0x%08lx", hr);
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
	s_fit = Fit();
}

static void draw_water()
{
	if (s_water_polys.empty() || !s_dev || !init_water_resources())
		return;

	IDirect3DSurface9 *rt = nullptr;
	if (FAILED(s_dev->GetRenderTarget(0, &rt)))
		return;
	D3DSURFACE_DESC rd;
	rt->GetDesc(&rd);
	if (!s_scene_tex || s_scene_w != rd.Width || s_scene_h != rd.Height) {
		if (s_scene_tex) s_scene_tex->Release();
		s_scene_tex = nullptr;
		if (FAILED(s_dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, &s_scene_tex, nullptr))) {
			log_info("mm7fx: scene copy texture %ux%u fmt %d failed", rd.Width, rd.Height, rd.Format);
			rt->Release();
			return;
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
	if (FAILED(hr)) {
		log_info("mm7fx: StretchRect failed 0x%08lx", hr);
		return;
	}

	solve_horizon();
	const float horizon_uv = s_horizon_px >= 0 ? s_horizon_px / rd.Height : 0.35f;
	const float params[4] = {GetTickCount() * 0.001f, horizon_uv, 0.75f, 0.0f};

	s_state->Capture();
	s_dev->SetVertexShader(nullptr);
	s_dev->SetPixelShader(s_water_ps);
	s_dev->SetPixelShaderConstantF(0, params, 1);
	s_dev->SetFVF(WaterFVF);
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
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
	for (const auto &p : s_water_polys) {
		s_dev->SetTexture(1, p.tex);
		s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, p.count - 2, &s_water_verts[p.first], sizeof(WaterVertex));
	}
	s_state->Apply();
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
	s_current_poly = nullptr;
}

using EndSceneFn = void(__fastcall *)(void *self, void *edx);
static EndSceneFn s_orig_end_scene = nullptr;

static void __fastcall hook_end_scene(void *self, void *edx)
{
	if (*reinterpret_cast<int *>(static_cast<uint8_t *>(self) + mm7::RenderSceneDepth) == 1) {
		if (s_mode == Mode::Effect)
			draw_water();
		release_captured();
	}
	s_orig_end_scene(self, edx);
}

// ---- ReShade events --------------------------------------------------------------------------

static bool is_water(const uint8_t *poly)
{
	return *reinterpret_cast<const int16_t *>(poly + mm7::PolygonFlags) & mm7::TileWater;
}

static bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
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
	case Mode::Effect:
		if (!s_poly_captured) {
			s_poly_captured = true;
			capture_water_poly();
		}
		return true; // redrawn later by draw_water()
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

	const DWORD now = GetTickCount();
	if (now - s_last_log >= 5000) {
		s_last_log = now;
		log_info("mm7fx: frame %u: %u draws, %u terrain, %u water, horizon y %.1f px, mode %d",
			s_frame, s_draws, s_terrain_draws, s_water_draws, s_horizon_px, static_cast<int>(s_mode));
	}
	s_draws = s_water_draws = s_terrain_draws = 0;
	release_captured(); // safety: never carry captures across frames
}

static void on_destroy_device(device *)
{
	release_captured();
	if (s_state) s_state->Release();
	if (s_water_ps) s_water_ps->Release();
	if (s_scene_tex) s_scene_tex->Release();
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
