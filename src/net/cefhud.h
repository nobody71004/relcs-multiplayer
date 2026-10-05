// src/net/cefhud.h — game-side glue for the Chromium (CEF) UI overlay.
//
// cefui.dll is loaded DYNAMICALLY so the game builds and runs fine without
// the CEF distribution present. When the DLL is missing every call is a no-op
// and the native CFont HUD remains the UI.
#pragma once

// called once the game window exists
void CefHud_Ensure(void);
bool CefHud_Active(void);      // DLL loaded + browser created
bool CefHud_Focused(void);     // UI is capturing input

void CefHud_Tick(void);        // pump CEF + upload fresh frames
void CefHud_Draw(void);        // draw the overlay quad (call last)

void CefHud_Show(const char* panel); // "chat" | "console" | "inventory" | ""
void CefHud_PushState(const char* json);
void CefHud_SetQueryHandler(void (*fn)(const char* request, void* user));

void CefHud_Key(int down, int vk, int ch, int mods);
void CefHud_Mouse(int type, int x, int y, int btn, int mods);
void CefHud_Shutdown(void);
