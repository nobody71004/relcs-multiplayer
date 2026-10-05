// src/net/cefhud.cpp — loads cefui.dll at runtime and renders the Chromium
// UI overlay (gameui.html) as a translucent texture over the game.
#include "common.h"
#include "Sprite2d.h"
#include "skeleton.h"

#include "cefhud.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

// --- cefui.dll C API (subset, via GetProcAddress) ---------------------------
typedef int  (*fn_api_version)(void);
typedef int  (*fn_init_osr)(void*, int, int, const char*);
typedef void (*fn_tick)(void);
typedef int  (*fn_paint)(const void**, int*, int*);
typedef void (*fn_resize)(int, int);
typedef void (*fn_focus)(int);
typedef void (*fn_key)(int, int, int, int);
typedef void (*fn_mouse)(int, int, int, int, int);
typedef void (*fn_exec)(const char*);
typedef void (*fn_query_handler)(void (*)(const char*, void*), void*);
typedef void (*fn_shutdown)(void);

static HMODULE s_dll = nullptr;
static fn_init_osr       p_init_osr = nullptr;
static fn_tick           p_tick = nullptr;
static fn_paint          p_paint = nullptr;
static fn_focus          p_focus = nullptr;
static fn_key            p_key = nullptr;
static fn_mouse          p_mouse = nullptr;
static fn_exec           p_exec = nullptr;
static fn_query_handler  p_query_handler = nullptr;
static fn_shutdown       p_shutdown = nullptr;

static bool s_ready = false;
static bool s_focused = false;
static bool s_initTried = false;

// overlay texture (BGRA frames uploaded from CEF)
static RwRaster* s_raster = nil;
static RwTexture* s_texture = nil;
static int s_texW = 0, s_texH = 0;

static void (*s_hostHandler)(const char*, void*) = nullptr;

static void HostTrampoline(const char* request, void*)
{
	if(s_hostHandler) s_hostHandler(request, nullptr);
}

bool CefHud_Active(void)  { return s_ready; }
bool CefHud_Focused(void) { return s_focused; }

void CefHud_Ensure(void)
{
	if(s_initTried) return;
	s_initTried = true;

	s_dll = LoadLibraryA("cefui.dll");
	if(!s_dll){
		fprintf(stderr, "[CEF] cefui.dll not found — native HUD only\n");
		fflush(stderr);
		return;
	}
	fn_api_version p_version = (fn_api_version)GetProcAddress(s_dll, "cefui_api_version");
	p_init_osr      = (fn_init_osr)GetProcAddress(s_dll, "cefui_init_osr");
	p_tick          = (fn_tick)GetProcAddress(s_dll, "cefui_tick");
	p_paint         = (fn_paint)GetProcAddress(s_dll, "cefui_paint");
	p_focus         = (fn_focus)GetProcAddress(s_dll, "cefui_focus");
	p_key           = (fn_key)GetProcAddress(s_dll, "cefui_key");
	p_mouse         = (fn_mouse)GetProcAddress(s_dll, "cefui_mouse");
	p_exec          = (fn_exec)GetProcAddress(s_dll, "cefui_exec");
	p_query_handler = (fn_query_handler)GetProcAddress(s_dll, "cefui_set_query_handler");
	p_shutdown      = (fn_shutdown)GetProcAddress(s_dll, "cefui_shutdown");
	if(!p_version || !p_init_osr || !p_tick || !p_paint){
		fprintf(stderr, "[CEF] cefui.dll is missing exports\n");
		fflush(stderr);
		FreeLibrary(s_dll);
		s_dll = nullptr;
		return;
	}
	fprintf(stderr, "[CEF] cefui.dll loaded (api v%d)\n", p_version());
	fflush(stderr);

	// gameui.html lives next to reLCS.exe
	char url[512] = {};
	char exedir[320] = {};
	GetModuleFileNameA(nullptr, exedir, sizeof exedir);
	char* slash = strrchr(exedir, '\\');
	if(slash) *slash = 0;
	snprintf(url, sizeof url, "file:///%s/gameui.html", exedir);
	for(char* c = url; *c; c++) if(*c == '\\') *c = '/';

	if(p_query_handler)
		p_query_handler(HostTrampoline, nullptr);

	HWND wnd = GetActiveWindow();
	if(!wnd) wnd = GetForegroundWindow();
	int w = RsGlobal.maximumWidth, h = RsGlobal.maximumHeight;
	if(!p_init_osr(wnd, w, h, url)){
		fprintf(stderr, "[CEF] init_osr failed\n");
		fflush(stderr);
		return;
	}
	s_ready = true;
	fprintf(stderr, "[CEF] overlay live: %s (%dx%d)\n", url, w, h);
	fflush(stderr);
}

void CefHud_SetQueryHandler(void (*fn)(const char*, void*))
{
	s_hostHandler = fn;
	if(s_ready && p_query_handler)
		p_query_handler(HostTrampoline, nullptr);
}

static void UploadFrame(void)
{
	const void* pixels = nullptr;
	int w = 0, h = 0;
	if(!p_paint(&pixels, &w, &h) || !pixels || w <= 0 || h <= 0)
		return;

	if(!s_texture || s_texW != w || s_texH != h){
		if(s_texture){ RwTextureDestroy(s_texture); s_texture = nil; s_raster = nil; }
		s_raster = RwRasterCreate(w, h, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
		if(!s_raster) return;
		s_texture = RwTextureCreate(s_raster);
		s_texW = w; s_texH = h;
	}

	uint8* dst = RwRasterLock(s_raster, 0, rwRASTERLOCKWRITE | rwRASTERLOCKRAW);
	if(!dst) return;
	const uint8* src = (const uint8*)pixels;
	for(int y = 0; y < h; y++){
		std::memcpy(dst + (size_t)y * s_raster->stride,
		            src + (size_t)y * w * 4, (size_t)w * 4);
	}
	RwRasterUnlock(s_raster);
}

void CefHud_Tick(void)
{
	if(!s_ready) return;
	p_tick();
	UploadFrame();
}

void CefHud_Draw(void)
{
	if(!s_ready || !s_texture) return;
	CSprite2d spr;
	spr.m_pTexture = s_texture;
	spr.Draw(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT), CRGBA(255, 255, 255, 255));
	spr.m_pTexture = nil; // don't destroy the shared texture
}

void CefHud_Show(const char* panel)
{
	if(!s_ready || !p_exec) return;
	char js[128];
	snprintf(js, sizeof js, "window.GameUI && GameUI.show('%s')", panel ? panel : "");
	p_exec(js);
	s_focused = panel && panel[0];
	if(p_focus) p_focus(s_focused ? 1 : 0);
}

void CefHud_PushState(const char* json)
{
	if(!s_ready || !p_exec || !json) return;
	// Escape for JS string literal: wrap json in a single-quoted string
	std::string esc;
	esc.reserve(std::strlen(json) + 32);
	for(const char* c = json; *c; c++){
		if(*c == '\'' || *c == '\\') esc += '\\';
		if(*c == '\n'){ esc += "\\n"; continue; }
		esc += *c;
	}
	std::string js = "window.GameUI && GameUI.push('" + esc + "')";
	p_exec(js.c_str());
}

void CefHud_Key(int down, int vk, int ch, int mods)
{
	if(s_ready && p_key) p_key(down, vk, ch, mods);
}

void CefHud_Mouse(int type, int x, int y, int btn, int mods)
{
	if(s_ready && p_mouse) p_mouse(type, x, y, btn, mods);
}

void CefHud_Shutdown(void)
{
	if(s_ready && p_shutdown) p_shutdown();
	if(s_texture){ RwTextureDestroy(s_texture); s_texture = nil; s_raster = nil; }
	if(s_dll){ FreeLibrary(s_dll); s_dll = nullptr; }
	s_ready = false;
}
