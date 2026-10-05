// net/cefui/cefui_impl.h — internal entry points shared by the DLL exports
// and the subprocess exe. Requires C++20 + CEF includes.
#pragma once
#include <string>

int  CefUi_ExecuteProcess(void* hInstance);
int  CefUi_InitOsr(void* hwnd, int w, int h, const std::string& url);
int  CefUi_InitWindowed(void* parent, int x, int y, int w, int h, const std::string& url);
void CefUi_Tick(void);
void CefUi_RunLoop(void);
int  CefUi_Paint(const void** pixels, int* w, int* h);
void CefUi_Resize(int w, int h);
void CefUi_Focus(int on);
void CefUi_Key(int down, int vk, int ch, int mods);
void CefUi_Mouse(int type, int x, int y, int btn, int mods);
void CefUi_Exec(const char* js);
void CefUi_SetQueryHandler(void (*fn)(const char*, void*), void* user);
void CefUi_CloseBrowser(void);
void CefUi_Shutdown(void);
