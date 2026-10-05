// src/net/nethooks.h — game glue for reLCS multiplayer.
#pragma once

// Call once per command-line argument (rsCOMMANDLINE event).
// Returns true when the argument was consumed by the net layer.
bool NetGame_OnCommandLine(const char* arg);
// Call once per frame (from Idle).
void NetGame_Frame(void);
bool NetGame_WantMouse(void); // UI panel owns the mouse (free OS cursor, no gameplay look)
// Call once per frame while the frontend menu is up (from FrontendIdle).
// Auto-starts a new game when -connect was given so joining a server never
// depends on menu navigation.
void NetGame_FrontendTick(void);
// Call from CHud::Draw for the chat/score overlay.
void NetGame_DrawHud(void);
// True while a multiplayer session is active.
bool NetGame_IsActive(void);
// Graceful teardown.
void NetGame_Shutdown(void);
