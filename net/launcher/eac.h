// net/launcher/eac.h — Easy Anti-Cheat bootstrap for the launcher.
//
// EAC ships as a bootstrapper (`start_protected_game.exe`) plus the
// `EasyAntiCheat/` runtime and a per-product config. The launcher installs that
// payload into the game directory, writes the EOS product config, builds the
// signed integrity catalogue with the product keys, and starts the game through
// the protected bootstrapper instead of the bare executable.
#pragma once

#include <string>

namespace eac {

struct Status {
	bool runtimeInstalled = false;   // EasyAntiCheat/ + start_protected_game.exe present
	bool configWritten = false;      // Settings.json + <game>.exe.eac written
	bool integrityBuilt = false;     // Certificates/base.bin from the signing tool
	bool staleIntegrity = false;     // base.bin signed before the exe/data changed
	bool eosSdk = false;             // EOSSDK-Win64-Shipping.dll in the game folder
	bool eosConfig = false;          // eos.ini (client credentials) in the game folder
	std::string version;             // runtime version from EasyAntiCheat/.version
	std::string lastError;
	std::string log;                 // last action summary for the UI
};

// Copies the runtime into gameDir and writes the product config. Idempotent.
bool Prepare(const std::string& gameDir, const std::string& gameExeName, Status& out);

// Runs EasyAntiCheat_EOS_Setup.exe install <productId> (may require elevation).
bool InstallService(const std::string& gameDir, Status& out);

// Generates EasyAntiCheat/Certificates/base.bin with base_private.key/base_public.cer.
bool BuildIntegrity(const std::string& gameDir, const std::string& gameExeName, Status& out);

// Starts the game with the given arguments. With `allowProtected` the EAC
// bootstrapper (start_protected_game.exe) is used when present; otherwise the
// game executable is started directly. Returns false and fills `error` when
// neither process could be created.
//
// `allowProtected` is opt-in: the protected bootstrapper runs the game in a
// sandbox that refuses to load the game's own runtime DLLs (cefui.dll and
// EOSSDK-Win64-Shipping.dll), which kills the Chromium HUD and the EOS session.
bool Launch(const std::string& gameDir, const std::string& gameExeName,
            const std::string& args, std::string& error, bool& protectedLaunch,
            bool allowProtected);

} // namespace eac
