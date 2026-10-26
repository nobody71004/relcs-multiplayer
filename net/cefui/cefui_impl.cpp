// net/cefui/cefui_impl.cpp — the real Chromium Embedded Framework UI stack.
//
// One CefApp/Client pair serves both modes:
//   windowless (OSR)  — game overlay; OnPaint delivers BGRA frames
//   windowed          — launcher; CEF owns a child window
//
// JS -> host traffic uses the "lcs://" scheme intercepted before navigation
// (no render-process customization needed, so the subprocess exe stays tiny).
// Host -> JS is ExecuteJavaScript.

#include "cefui_impl.h"
#include "cefui.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_callback.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// shared state
// ---------------------------------------------------------------------------
static cefui_query_fn g_queryFn = nullptr;
static void* g_queryUser = nullptr;

static std::atomic<bool> g_osrMode{false};
static std::atomic<bool> g_focus{false};
static int g_w = 1280, g_h = 720;

static std::mutex g_paintLock;
static std::vector<uint8_t> g_paintBuf;
static int g_paintW = 0, g_paintH = 0;
static bool g_paintDirty = false;

static std::atomic<bool> g_closing{false};
static CefRefPtr<CefBrowser> g_browser;

// ExecuteJavaScript must run on the CEF UI thread — host replies may come
// from worker threads (server queries), so everything marshals through tasks
class ExecTask : public CefTask
{
public:
	explicit ExecTask(const std::string& js) : m_js(js) {}
	void Execute() override
	{
		if(g_browser)
			g_browser->GetMainFrame()->ExecuteJavaScript(m_js, "lcs://exec", 0);
	}
private:
	std::string m_js;
	IMPLEMENT_REFCOUNTING(ExecTask);
};

static void DispatchQuery(const std::string& url)
{
	// lcs://command?args  ->  "command?args" (decoded)
	std::string req = url;
	size_t sch = req.find("lcs://");
	if(sch != 0) return;
	req = req.substr(6);
	// strip any fragment
	size_t hash = req.find('#');
	if(hash != std::string::npos) req.resize(hash);
	CefString decoded = CefURIDecode(CefString(req), true, UU_SPACES);
	if(g_queryFn)
		g_queryFn(decoded.ToString().c_str(), g_queryUser);
}

// lcs:// routing also has to survive resource-level navigation (iframe src
// changes land here in modern CEF)
class UiResourceRequestHandler : public CefResourceRequestHandler
{
public:
	ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
	                                CefRefPtr<CefFrame> frame,
	                                CefRefPtr<CefRequest> request,
	                                CefRefPtr<CefCallback> callback) override
	{
		std::string url = request->GetURL();
		if(url.rfind("lcs://", 0) == 0){
			DispatchQuery(url);
			return RV_CANCEL;
		}
		return RV_CONTINUE;
	}
	IMPLEMENT_REFCOUNTING(UiResourceRequestHandler);
};

// ---------------------------------------------------------------------------
// client
// ---------------------------------------------------------------------------
class UiClient : public CefClient,
                 public CefLifeSpanHandler,
                 public CefRenderHandler,
                 public CefRequestHandler,
                 public CefLoadHandler,
                 public CefDisplayHandler
{
public:
	// CefClient
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return g_osrMode ? this : nullptr; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
	CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
	    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	    CefRefPtr<CefRequest> request, bool is_navigation, bool is_download,
	    const CefString& request_initiator, bool& disable_default_handling) override
	{
		if(!m_resHandler) m_resHandler = new UiResourceRequestHandler();
		return m_resHandler;
	}
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

	// CefLifeSpanHandler
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
	{
		g_browser = browser;
	}
	bool DoClose(CefRefPtr<CefBrowser> browser) override
	{
		return false;
	}
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
	{
		g_browser = nullptr;
		CefQuitMessageLoop();
	}

	// CefRenderHandler (OSR)
	void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
	{
		rect = CefRect(0, 0, g_w, g_h);
	}
	bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override
	{
		info.rect = CefRect(0, 0, g_w, g_h);
		info.available_rect = info.rect;
		info.depth = 32;
		info.device_scale_factor = 1.0f;
		return true;
	}
	void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
	             const RectList& dirtyRects, const void* pixels,
	             int width, int height) override
	{
		if(type != PET_VIEW) return;
		std::lock_guard<std::mutex> guard(g_paintLock);
		if(g_paintW != width || g_paintH != height){
			g_paintW = width;
			g_paintH = height;
			g_paintBuf.resize((size_t)width * height * 4);
		}
		std::memcpy(g_paintBuf.data(), pixels, (size_t)width * height * 4);
		g_paintDirty = true;
	}

	// CefRequestHandler — lcs:// routing
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
	                    CefRefPtr<CefFrame> frame,
	                    CefRefPtr<CefRequest> request,
	                    bool user_gesture,
	                    bool is_redirect) override
	{
		std::string url = request->GetURL();
		if(url.rfind("lcs://", 0) == 0){
			DispatchQuery(url);
			return true; // cancel the navigation, page stays
		}
		return false;
	}
private:
	CefRefPtr<UiResourceRequestHandler> m_resHandler;

public:

	// CefLoadHandler
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	               int httpStatusCode) override
	{
		if(!frame->IsMain()) return;
		if(g_queryFn)
			g_queryFn("loaded", g_queryUser);
	}
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	                 ErrorCode errorCode, const CefString& errorText,
	                 const CefString& failedUrl) override
	{
		if(!frame->IsMain()) return;
		std::string msg = "loaderror " + errorText.ToString() + " " + failedUrl.ToString();
		if(g_queryFn)
			g_queryFn(msg.c_str(), g_queryUser);
	}

	// CefDisplayHandler
	void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override
	{
		std::string msg = "title " + title.ToString();
		if(g_queryFn)
			g_queryFn(msg.c_str(), g_queryUser);
	}

	IMPLEMENT_REFCOUNTING(UiClient);
};

// ---------------------------------------------------------------------------
// app
// ---------------------------------------------------------------------------
class UiApp : public CefApp
{
public:
	void OnBeforeCommandLineProcessing(const CefString& process_type,
	                                   CefRefPtr<CefCommandLine> command_line) override
	{
		// robustness first: software fallbacks keep the overlay alive on
		// machines where GPU compositing misbehaves inside the game's d3d9
		command_line->AppendSwitch("disable-gpu-compositing");
		command_line->AppendSwitch("disable-gpu-shader-disk-cache");
		command_line->AppendSwitchWithValue("disable-features",
		                                   "CalculateNativeWinOcclusion");
	}
	IMPLEMENT_REFCOUNTING(UiApp);
};

static CefRefPtr<UiApp> g_app;
static CefRefPtr<UiClient> g_client;

// ---------------------------------------------------------------------------
// internal entry points (cefui_impl.h)
// ---------------------------------------------------------------------------
int CefUi_ExecuteProcess(void* hInstance)
{
	CefMainArgs args((HINSTANCE)hInstance);
	return CefExecuteProcess(args, nullptr, nullptr);
}

static std::string ModuleDir()
{
	char path[MAX_PATH] = {};
	HMODULE hm = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&ModuleDir, &hm);
	GetModuleFileNameA(hm, path, MAX_PATH);
	std::string dir(path);
	size_t slash = dir.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
}

// Keep this handler dependency-free: dbghelp is loaded dynamically (a link
// dependency would complicate the cefui build) and the exception struct is
// defined locally to avoid requiring dbghelp.h.
typedef struct RelcsMiniDumpExcInfo {
	unsigned long ThreadId;
	EXCEPTION_POINTERS* ExceptionPointers;
	int ClientPointers;
} RelcsMiniDumpExcInfo;

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
	// The raw absolute address is useless after restart (ASLR) — also log the
	// module name + base so the offset can be resolved against reLCS.map.
	void* addr = ep->ExceptionRecord->ExceptionAddress;
	HMODULE hm = nullptr;
	char mod[MAX_PATH] = "?";
	const char* modName = mod;
	if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                      (LPCSTR)addr, &hm)){
		GetModuleFileNameA(hm, mod, MAX_PATH);
		const char* slash = strrchr(mod, '\\');
		if(slash) modName = slash + 1;
		fprintf(stderr, "[CEF] CRASH code=0x%08lx addr=%p in %s base=%p rva=0x%Ix",
		        ep->ExceptionRecord->ExceptionCode, addr, modName,
		        (void*)hm, (size_t)((char*)addr - (char*)hm));
	}else
		fprintf(stderr, "[CEF] CRASH code=0x%08lx addr=%p",
		        ep->ExceptionRecord->ExceptionCode, addr);
	// for access violations the interesting part is WHAT was dereferenced
	if(ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
	   ep->ExceptionRecord->NumberParameters >= 2)
		fprintf(stderr, " %s %p",
		        ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
		        (void*)ep->ExceptionRecord->ExceptionInformation[1]);
	fprintf(stderr, "\n");
	fflush(stderr);

	// full minidump so the next crash arrives with a stack, not one address
	HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
	if(dbghelp){
		typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int,
		                                           RelcsMiniDumpExcInfo*, void*, void*);
		MiniDumpWriteDump_t pDump = (MiniDumpWriteDump_t)
			GetProcAddress(dbghelp, "MiniDumpWriteDump");
		if(pDump){
			char path[MAX_PATH] = {};
			// prefer the game folder (easy to find), fall back to %TEMP%
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			char* slash = strrchr(path, '\\');
			if(slash) *(slash + 1) = '\0';
			size_t n = strlen(path);
			snprintf(path + n, sizeof(path) - n, "relcs-crash-%lu.dmp", GetCurrentProcessId());
			HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			                       FILE_ATTRIBUTE_NORMAL, nullptr);
			if(f == INVALID_HANDLE_VALUE){
				char tmp[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, tmp);
				snprintf(path, sizeof path, "%srelcs-crash-%lu.dmp", tmp, GetCurrentProcessId());
				f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				               FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if(f != INVALID_HANDLE_VALUE){
				RelcsMiniDumpExcInfo info;
				info.ThreadId = GetCurrentThreadId();
				info.ExceptionPointers = ep;
				info.ClientPointers = 0;
				if(pDump(GetCurrentProcess(), GetCurrentProcessId(), f,
				         1 /*MiniDumpNormal*/, &info, nullptr, nullptr))
					fprintf(stderr, "[CEF] crash dump written: %s\n", path);
				else
					fprintf(stderr, "[CEF] crash dump failed: %lu\n", GetLastError());
				fflush(stderr);
				CloseHandle(f);
			}
		}
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

static bool CefUi_InitCommon(const std::string& url)
{
	SetUnhandledExceptionFilter(CrashFilter);
	fprintf(stderr, "[CEF] InitCommon: start\n"); fflush(stderr);
	HINSTANCE hi = GetModuleHandle(nullptr);
	CefMainArgs args(hi);

	CefSettings settings;
	settings.windowless_rendering_enabled = true;
	settings.multi_threaded_message_loop = false;
	settings.external_message_pump = false;
	settings.no_sandbox = true;
	settings.log_severity = LOGSEVERITY_WARNING;

	std::string dir = ModuleDir();
	// runtime files (libcef.dll, *.pak, icudtl.dat, locales/) live beside the
	// DLL; subprocesses are the dedicated tiny exe
	std::string subprocess = dir + "\\cefsub.exe";
	std::string resources = dir;
	std::string locales = dir + "\\locales";
	CefString(&settings.browser_subprocess_path).FromString(subprocess);
	CefString(&settings.resources_dir_path).FromString(resources);
	CefString(&settings.locales_dir_path).FromString(locales);
	// per-process cache dir: two hosts (game overlay + launcher) must never
	// collide over Chromium's profile singleton
	char tmp[MAX_PATH] = {};
	GetTempPathA(MAX_PATH, tmp);
	char cache[MAX_PATH] = {};
	snprintf(cache, sizeof cache, "%srelcs-cef-%lu", tmp, GetCurrentProcessId());
	CefString(&settings.root_cache_path).FromString(cache);

	fprintf(stderr, "[CEF] InitCommon: calling CefInitialize\n"); fflush(stderr);
	g_app = new UiApp();
	if(!CefInitialize(args, settings, g_app, nullptr)){
		fprintf(stderr, "[CEF] InitCommon: CefInitialize FAILED\n"); fflush(stderr);
		return false;
	}
	fprintf(stderr, "[CEF] InitCommon: CefInitialize ok\n"); fflush(stderr);
	g_client = new UiClient();
	return true;
}

int CefUi_InitOsr(void* hwnd, int w, int h, const std::string& url)
{
	g_osrMode = true;
	g_w = w; g_h = h;
	if(!CefUi_InitCommon(url))
		return 0;
	CefWindowInfo wi;
	wi.SetAsWindowless((CefWindowHandle)hwnd);
	CefBrowserSettings bs;
	bs.windowless_frame_rate = 30; // the HUD is mostly static; 60 fps doubled the per-frame paint and texture copy
	if(!CefBrowserHost::CreateBrowser(wi, g_client, url, bs, nullptr, nullptr))
		return 0;
	return 1;
}

int CefUi_InitWindowed(void* parent, int x, int y, int w, int h, const std::string& url)
{
	g_osrMode = false;
	g_w = w; g_h = h;
	if(!CefUi_InitCommon(url))
		return 0;
	CefWindowInfo wi;
	wi.SetAsChild((CefWindowHandle)parent, CefRect(x, y, w, h));
	// stick to the Alloy runtime like the OSR path — the Chrome runtime needs
	// far heavier host integration and crashes in the first message pump
	wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
	CefBrowserSettings bs;
	fprintf(stderr, "[CEF] InitWindowed: CreateBrowser\n"); fflush(stderr);
	if(!CefBrowserHost::CreateBrowser(wi, g_client, url, bs, nullptr, nullptr)){
		fprintf(stderr, "[CEF] InitWindowed: CreateBrowser FAILED\n"); fflush(stderr);
		return 0;
	}
	fprintf(stderr, "[CEF] InitWindowed: CreateBrowser ok\n"); fflush(stderr);
	return 1;
}

void CefUi_Tick(void)     { CefDoMessageLoopWork(); }
void CefUi_RunLoop(void)  { CefRunMessageLoop(); }

int CefUi_Paint(const void** pixels, int* w, int* h)
{
	std::lock_guard<std::mutex> guard(g_paintLock);
	if(!g_paintDirty || g_paintBuf.empty())
		return 0;
	*pixels = g_paintBuf.data();
	*w = g_paintW;
	*h = g_paintH;
	g_paintDirty = false;
	return 1;
}

void CefUi_Resize(int w, int h)
{
	g_w = w; g_h = h;
	if(!g_browser) return;
	if(g_osrMode)
		g_browser->GetHost()->WasResized();
	else{
		CefWindowHandle wh = g_browser->GetHost()->GetWindowHandle();
		if(wh)
			SetWindowPos((HWND)wh, nullptr, 0, 0, w, h, SWP_NOZORDER);
	}
}

void CefUi_Focus(int on)
{
	g_focus = on != 0;
	if(g_browser)
		g_browser->GetHost()->SetFocus(g_focus);
}

void CefUi_Key(int down, int vk, int ch, int mods)
{
	if(!g_browser) return;
	CefKeyEvent ke;
	ke.windows_key_code = ch ? ch : vk;
	ke.native_key_code = vk;
	ke.is_system_key = false;
	ke.modifiers = 0;
	if(mods & CEFUI_MOD_SHIFT) ke.modifiers |= EVENTFLAG_SHIFT_DOWN;
	if(mods & CEFUI_MOD_CTRL)  ke.modifiers |= EVENTFLAG_CONTROL_DOWN;
	if(mods & CEFUI_MOD_ALT)   ke.modifiers |= EVENTFLAG_ALT_DOWN;

	if(ch){
		if(!down) return; // character events fire once
		ke.type = KEYEVENT_CHAR;
		ke.windows_key_code = ch;
		ke.character = (char16_t)ch;
		ke.unmodified_character = (char16_t)ch;
		g_browser->GetHost()->SendKeyEvent(ke);
		return;
	}
	ke.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
	g_browser->GetHost()->SendKeyEvent(ke);
}

void CefUi_Mouse(int type, int x, int y, int btn, int mods)
{
	if(!g_browser) return;
	CefMouseEvent me;
	me.x = x;
	me.y = y;
	me.modifiers = 0;
	if(mods & CEFUI_MOD_SHIFT) me.modifiers |= EVENTFLAG_SHIFT_DOWN;
	if(mods & CEFUI_MOD_CTRL)  me.modifiers |= EVENTFLAG_CONTROL_DOWN;
	if(mods & CEFUI_MOD_ALT)   me.modifiers |= EVENTFLAG_ALT_DOWN;
	CefRefPtr<CefBrowserHost> host = g_browser->GetHost();
	switch(type){
	case 0: host->SendMouseMoveEvent(me, false); break;
	case 1: host->SendMouseClickEvent(me, (CefBrowserHost::MouseButtonType)btn, false, 1); break;
	case 2: host->SendMouseClickEvent(me, (CefBrowserHost::MouseButtonType)btn, true, 1); break;
	case 3: host->SendMouseWheelEvent(me, 0, btn); break;
	}
}

void CefUi_Exec(const char* js)
{
	if(js)
		CefPostTask(TID_UI, new ExecTask(js));
}

void CefUi_SetQueryHandler(cefui_query_fn fn, void* user)
{
	g_queryFn = fn;
	g_queryUser = user;
}

void CefUi_CloseBrowser(void)
{
	if(g_browser)
		g_browser->GetHost()->CloseBrowser(true);
}

void CefUi_Shutdown(void)
{
	// CefShutdown waits for every browser to close, but the game pumps CEF
	// messages from its main thread — inside CefShutdown that pump stops, so
	// the wait never completes and the window freezes on its last frame.
	// Close the browser and pump until OnBeforeClose clears it first.
	if(g_browser){
		g_browser->GetHost()->CloseBrowser(true);
		for(int i = 0; i < 400 && g_browser; i++){
			CefDoMessageLoopWork();
			Sleep(5);
		}
	}
	if(g_browser){
		// Never reached the closed state. Leave CEF (and its DLL) alive: a
		// stuck CefShutdown would hang the process, and the game is exiting
		// anyway — the OS reclaims the threads.
		return;
	}
	g_client = nullptr;
	CefShutdown();
	g_app = nullptr;
}
