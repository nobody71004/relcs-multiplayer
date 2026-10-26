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

// Starts start_protected_game.exe with the game arguments (falls back to the game
// executable when the bootstrapper is missing). Returns false and fills `error`
// when neither process could be created.
bool Launch(const std::string& gameDir, const std::string& gameExeName,
            const std::string& args, std::string& error, bool& protectedLaunch);

} // namespace eac
