// src/net/acclient.h — client side of the EOS Anti-Cheat session.
//
// The dedicated server runs EOS_AntiCheatServer and only lets *protected*
// clients play: a client must open its own EOS_AntiCheatClient session, register
// with the server (MSG_AC_HELLO) and then relay the kernel driver's traffic
// between the two sessions.
//
// The SDK is loaded at runtime (LoadLibrary / GetProcAddress), never linked:
// a game folder without EOSSDK-Win64-Shipping.dll still starts, it just plays
// unprotected and is refused by servers that require protection. That keeps the
// "copy the folder and run" install working, which a hard import would break.
#pragma once

#include <cstdint>
#include <string>

namespace acclient {

enum class State {
	Off = 0,        // not initialised yet
	SdkMissing,     // EOSSDK-Win64-Shipping.dll not found next to the game
	InitFailed,     // EOS_Initialize / platform creation refused
	LoggingIn,      // Connect device id + login in flight
	Failed,         // BeginSession (or login) failed: see Detail()
	Ready,          // AC session live: MSG_AC_HELLO can be sent
};

// EOS hands us a message that must reach the game server (MSG_AC_FROM_CLIENT).
// NetClient installs the sender; callbacks run inside Tick().
using ToServerFn = void (*)(const void* data, uint32_t len);
void SetSendToServer(ToServerFn fn);

// Starts the SDK, the Connect device-id login and the AC session. Idempotent
// and non-blocking: every async step completes inside Tick(). Call it when the
// player joins; calling it again after a failure retries the failed phase.
bool Init();
// Pumps EOS (platform tick + async steps). Call once per frame.
void Tick();
void Shutdown();

// Payload of a MSG_AC_TO_CLIENT packet from the server.
void OnServerMessage(const void* data, uint32_t len);

bool Ready();
State state();
const char* StateName();      // "ready", "sdk-missing", ...
const char* Detail();         // one line for the player/log; "" when Ready
const char* ProductUserId();  // "" until the Connect login completed

// Fresh Connect ID token for MSG_AC_HELLO. Tokens are short-lived, so this is
// asked for right before each registration attempt rather than cached.
bool CopyIdToken(std::string& out);

// Latest local integrity violation reported by EOS ("" = none). The server is
// told by EOS itself; the game shows this to the player.
const char* Violation();
void ClearViolation();

} // namespace acclient
