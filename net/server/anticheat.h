// net/server/anticheat.h — EOS Anti-Cheat server session.
//
// Wraps EOS_AntiCheatServer: platform creation, session begin/end, client
// registration (after verifying the client's Connect ID token) and the opaque
// message relay. The SDK is loaded at runtime, so a server image without the
// EOS library still boots — it just reports the anti-cheat as unavailable
// instead of failing to start.
#pragma once

#include "anticheat_config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ac {

enum class State : int {
	Off = 0,          // eac=0: integration disabled by config
	SdkMissing,       // no libEOSSDK / EOSSDK-Win64-Shipping.dll on the box
	NotConfigured,    // SDK loaded but no client credential pair (dev portal)
	InitFailed,       // EOS_Initialize / EOS_Platform_Create / BeginSession failed
	Running,          // session live: registration + relay available
};

const char* StateName(State s);

// Session implementation (SDK binding, session, client table). Defined in
// anticheat.cpp so this header never needs the EOS SDK.
struct Impl;

class Server {
public:
	// Log sink: one already-formatted line, no trailing newline.
	using LogFn = std::function<void(const std::string& line)>;
	// EOS wants to send AC traffic to a client (handle = what we registered).
	using MessageToClientFn = std::function<void(void* handle, const void* data, uint32_t len)>;
	// EOS says the client must be removed from the session. reasonCode is the
	// EOS_EAntiCheatCommonClientActionReason value (2 = InvalidMessage,
	// 3 = AuthenticationFailed, 6/7 = client/backend violation ...).
	using ActionRequiredFn = std::function<void(void* handle, int action, int reasonCode, const char* reason)>;
	// Is that handle still connected? (a registration may complete after the
	// peer already left — registering a dead peer would leak a session slot)
	using StillConnectedFn = std::function<bool(void* handle)>;
	// Registration finished (ok or not): the game server answers MSG_AC_STATUS.
	using RegisterResultFn = std::function<void(void* handle, int status, const std::string& detail)>;

	Server();
	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	void SetLog(LogFn fn);
	void SetCallbacks(MessageToClientFn toClient, ActionRequiredFn actionRequired,
	                  StillConnectedFn stillConnected, RegisterResultFn registerResult);

	// Loads the SDK, creates the platform and begins the AC session.
	// Returns true only when the session is live; on false, state()/detail()
	// say why and the server keeps running unprotected.
	bool Start(const Config& cfg);
	void Stop();

	// Pump EOS work (id-token verification callbacks, session keepalive).
	// Call once per server tick with the server clock.
	void Tick(double nowSeconds);
	bool TickDue(double nowSeconds) const;

	bool Ready() const;
	State state() const;
	const std::string& detail() const;
	const std::string& sdkVersion() const;
	const Config& config() const;

	// --- client registration ------------------------------------------------
	// Verifies the Connect ID token, then EOS_AntiCheatServer_RegisterClient.
	// Returns the immediate status (AC_REG_OK means "verification in flight").
	int RegisterClient(void* handle, const std::string& productUserId,
	                   const std::string& idToken, uint8_t platform,
	                   const std::string& ip);
	void UnregisterClient(void* handle);
	bool IsRegistered(void* handle) const;
	int ClientCount() const;

	// --- relay --------------------------------------------------------------
	void ReceiveFromClient(void* handle, const void* data, uint32_t len);

	// One human-readable status line for `ac status` / boot logging.
	std::string StatusLine() const;

	struct ClientInfo {
		void* handle = nullptr;
		std::string productUserId;
		std::string ip;
		uint8_t platform = 0;
		bool registered = false;   // EOS_AntiCheatServer_RegisterClient succeeded
		bool verifying = false;    // ID token check still in flight
	};
	std::vector<ClientInfo> Clients() const;

private:
	std::unique_ptr<Impl> m_impl;
};

} // namespace ac
