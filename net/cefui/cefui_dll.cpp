// net/cefui/cefui_dll.cpp — the C export surface of cefui.dll (see cefui.h).
#define CEFUI_EXPORTS
#include "cefui.h"
#include "cefui_impl.h"

extern "C" {

int cefui_api_version(void) { return CEFUI_API_VERSION; }

int cefui_execute_process(void* hInstance)
{
	return CefUi_ExecuteProcess(hInstance);
}

int cefui_init_osr(void* hwnd, int w, int h, const char* url)
{
	return CefUi_InitOsr(hwnd, w, h, url ? url : "about:blank");
}

int cefui_init_windowed(void* parentHwnd, int x, int y, int w, int h, const char* url)
{
	return CefUi_InitWindowed(parentHwnd, x, y, w, h, url ? url : "about:blank");
}

void cefui_tick(void)   { CefUi_Tick(); }
void cefui_run_loop(void) { CefUi_RunLoop(); }

int cefui_paint(const void** pixels, int* w, int* h)
{
	return CefUi_Paint(pixels, w, h);
}

void cefui_resize(int w, int h) { CefUi_Resize(w, h); }
void cefui_focus(int on)        { CefUi_Focus(on); }
void cefui_key(int down, int vk, int ch, int mods) { CefUi_Key(down, vk, ch, mods); }
void cefui_mouse(int type, int x, int y, int btn, int mods) { CefUi_Mouse(type, x, y, btn, mods); }
void cefui_exec(const char* js) { CefUi_Exec(js); }
void cefui_set_query_handler(cefui_query_fn fn, void* userData)
{
	CefUi_SetQueryHandler((void (*)(const char*, void*))fn, userData);
}
void cefui_close_browser(void) { CefUi_CloseBrowser(); }
void cefui_shutdown(void)      { CefUi_Shutdown(); }

} // extern "C"
