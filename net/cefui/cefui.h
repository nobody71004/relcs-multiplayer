/* net/cefui/cefui.h — C API of cefui.dll: the real Chromium (CEF) UI stack.
 *
 * Two consumers:
 *   - reLCS.exe      : OSR (windowless) mode — the DLL paints into a BGRA
 *                      buffer that the game uploads as a texture overlay
 *   - reLCS-launcher : windowed mode — CEF owns a child window
 *
 * Both render the same HTML/JS/CSS stack. JS talks to the host through a
 * custom URL scheme:  window.location (or a hidden iframe) pointing at
 *   lcs://<command>?<args...>
 * is intercepted before the navigation and delivered to the registered query
 * handler. The host answers with cefui_exec("onCefReply(...)").
 */
#pragma once

#ifdef CEFUI_EXPORTS
#define CEFUI_API __declspec(dllexport)
#else
#define CEFUI_API __declspec(dllexport) /* consumers use LoadLibrary/GetProcAddress */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* JS -> host: request is "<command>?<args>" (URL-decoded), delivered on the CEF UI thread */
typedef void (*cefui_query_fn)(const char* request, void* userData);

/* version handshake: returns CEFUI_API_VERSION */
CEFUI_API int  cefui_api_version(void);
#define CEFUI_API_VERSION 1

/* called once at process start (before any init). Runs CefExecuteProcess for
 * the subprocess case: returns >= 0 when THIS process is a CEF subprocess and
 * the caller must exit with that code. Returns -1 for a normal host process. */
CEFUI_API int  cefui_execute_process(void* hInstance);

/* windowless (game overlay). url must be http(s)/file. */
CEFUI_API int  cefui_init_osr(void* hwnd, int w, int h, const char* url);

/* windowed (launcher). Creates a child window of parentHwnd. */
CEFUI_API int  cefui_init_windowed(void* parentHwnd, int x, int y, int w, int h, const char* url);

/* pumps CEF's message loop — call every frame (OSR) or not at all when using
 * cefui_run_loop (windowed) */
CEFUI_API void cefui_tick(void);

/* blocks in CefRunMessageLoop (windowed apps without their own pump) */
CEFUI_API void cefui_run_loop(void);

/* OSR paint: returns 1 and fills the buffer when a fresh frame is available.
 * pixels is BGRA, tightly packed, valid until the next cefui_tick. */
CEFUI_API int  cefui_paint(const void** pixels, int* w, int* h);

CEFUI_API void cefui_resize(int w, int h);
CEFUI_API void cefui_focus(int on); /* OSR: route keyboard/mouse to CEF */

/* keyboard: down=1 press, 0 release; vk = windows VK; ch = character (0 for
 * pure keys); mods = cefui_mod_* bitmask */
#define CEFUI_MOD_SHIFT 1
#define CEFUI_MOD_CTRL  2
#define CEFUI_MOD_ALT   4
CEFUI_API void cefui_key(int down, int vk, int ch, int mods);

/* mouse: type 0=move 1=down 2=up 3=wheel (btn = 0 left, 1 middle, 2 right,
 * wheel: btn = wheel delta) */
CEFUI_API void cefui_mouse(int type, int x, int y, int btn, int mods);

CEFUI_API void cefui_exec(const char* js);
CEFUI_API void cefui_set_query_handler(cefui_query_fn fn, void* userData);
CEFUI_API void cefui_close_browser(void);
CEFUI_API void cefui_shutdown(void);

#ifdef __cplusplus
}
#endif
