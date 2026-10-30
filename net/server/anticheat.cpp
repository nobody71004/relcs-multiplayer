// net/server/anticheat.cpp — EOS Anti-Cheat server session (EOS SDK 1.19.x).
//
// The SDK is bound at runtime (LoadLibrary / dlopen) rather than linked, so:
//   * a server image built once runs with or without libEOSSDK on the box, and
//   * a fleet host that has not been given the library yet still serves players
//     (it reports the anti-cheat as unavailable instead of refusing to boot).
//
// Flow, mirroring the EOS anti-cheat sample server:
//   EOS_Initialize -> EOS_Platform_Create(bIsServer) -> GetAntiCheatServerInterface
//   -> AddNotify{MessageToClient,ClientActionRequired,ClientAuthStatusChanged}
//   -> BeginSession
//   -> per peer: Connect_VerifyIdToken -> AntiCheatServer_RegisterClient
//   -> relay: ReceiveMessageFromClient  <->  OnMessageToClient
//
// Everything here must run on one thread: EOS delivers its callbacks from
// inside EOS_Platform_Tick, which the server loop calls from Server::Tick.
#include "anticheat.h"

#include "compat.h"        // windows.h / POSIX shims (+ _stricmp)
#include "rnet_common.h"   // MAX_AC_PAYLOAD

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <eos_sdk.h>
#include <eos_init.h>
#include <eos_common.h>
#include <eos_logging.h>
#include <eos_connect.h>
#include <eos_anticheatserver.h>
#include <eos_anticheatcommon_types.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#endif

namespace ac {

// One AC session per process: the EOS log callback carries no user data.
namespace {
Impl* g_impl = nullptr;

void* LibOpen(const std::string& path)
{
#ifdef _WIN32
	return (void*)LoadLibraryA(path.c_str());
#else
	return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void LibClose(void* lib)
{
	if(!lib) return;
#ifdef _WIN32
	FreeLibrary((HMODULE)lib);
#else
	dlclose(lib);
#endif
}

void* LibSym(void* lib, const char* name)
{
	if(!lib) return nullptr;
#ifdef _WIN32
	return (void*)(uintptr_t)GetProcAddress((HMODULE)lib, name);
#else
	return dlsym(lib, name);
#endif
}

std::string AbsolutePath(const std::string& rel)
{
#ifdef _WIN32
	char buf[MAX_PATH * 2] = {};
	if(_fullpath(buf, rel.c_str(), sizeof buf - 1)) return std::string(buf);
#else
	if(!rel.empty() && rel[0] == '/') return rel;
	char cwd[PATH_MAX] = {};
	if(getcwd(cwd, sizeof cwd)) return std::string(cwd) + "/" + rel;
#endif
	return rel;
}

void MakeDir(const std::string& dir)
{
#ifdef _WIN32
	CreateDirectoryA(dir.c_str(), nullptr);
#else
	mkdir(dir.c_str(), 0755);
#endif
}

std::string EnvVar(const char* name)
{
	const char* v = std::getenv(name);
	return v ? std::string(v) : std::string();
}

} // namespace

// ---------------------------------------------------------------------------
const char* StateName(State s)
{
	switch(s){
	case State::Off:           return "off";
	case State::SdkMissing:    return "sdk-missing";
	case State::NotConfigured: return "not-configured";
	case State::InitFailed:    return "init-failed";
	case State::Running:       return "running";
	}
	return "?";
}

// ---------------------------------------------------------------------------
struct Impl {
	Config cfg;
	State state = State::Off;
	std::string detail = "not started";
	std::string sdkVersion = "1.19.2";
	std::string libPath;
	// last EOS-side error, surfaced by `ac status`: the common one is a dev
	// portal permission gap (403 policy_missing_action) that the local
	// BeginSession cannot detect.
	std::string lastError;

	Server::LogFn logFn;
	Server::MessageToClientFn toClientFn;
	Server::ActionRequiredFn actionRequiredFn;
	Server::StillConnectedFn stillConnectedFn;
	Server::RegisterResultFn registerResultFn;

	// --- dynamic SDK entry points ------------------------------------------
	void* lib = nullptr;
	decltype(&EOS_Initialize) Initialize = nullptr;
	decltype(&EOS_Shutdown) Shutdown = nullptr;
	decltype(&EOS_Platform_Create) Platform_Create = nullptr;
	decltype(&EOS_Platform_Release) Platform_Release = nullptr;
	decltype(&EOS_Platform_Tick) Platform_Tick = nullptr;
	decltype(&EOS_Platform_GetConnectInterface) GetConnectInterface = nullptr;
	decltype(&EOS_Platform_GetAntiCheatServerInterface) GetAntiCheatServerInterface = nullptr;
	decltype(&EOS_Logging_SetCallback) Logging_SetCallback = nullptr;
	decltype(&EOS_Logging_SetLogLevel) Logging_SetLogLevel = nullptr;
	decltype(&EOS_EResult_ToString) ResultToString = nullptr;
	decltype(&EOS_ProductUserId_FromString) ProductUserId_FromString = nullptr;
	decltype(&EOS_ProductUserId_ToString) ProductUserId_ToString = nullptr;
	decltype(&EOS_Connect_VerifyIdToken) VerifyIdToken = nullptr;
	decltype(&EOS_AntiCheatServer_AddNotifyMessageToClient) AddNotifyMessageToClient = nullptr;
	decltype(&EOS_AntiCheatServer_RemoveNotifyMessageToClient) RemoveNotifyMessageToClient = nullptr;
	decltype(&EOS_AntiCheatServer_AddNotifyClientActionRequired) AddNotifyClientActionRequired = nullptr;
	decltype(&EOS_AntiCheatServer_RemoveNotifyClientActionRequired) RemoveNotifyClientActionRequired = nullptr;
	decltype(&EOS_AntiCheatServer_AddNotifyClientAuthStatusChanged) AddNotifyClientAuthStatusChanged = nullptr;
	decltype(&EOS_AntiCheatServer_RemoveNotifyClientAuthStatusChanged) RemoveNotifyClientAuthStatusChanged = nullptr;
	decltype(&EOS_AntiCheatServer_BeginSession) BeginSession = nullptr;
	decltype(&EOS_AntiCheatServer_EndSession) EndSession = nullptr;
	decltype(&EOS_AntiCheatServer_RegisterClient) RegisterClientFn = nullptr;
	decltype(&EOS_AntiCheatServer_UnregisterClient) UnregisterClientFn = nullptr;
	decltype(&EOS_AntiCheatServer_ReceiveMessageFromClient) ReceiveMessageFromClient = nullptr;

	// --- live objects ------------------------------------------------------
	EOS_HPlatform platform = nullptr;
	EOS_HConnect connect = nullptr;
	EOS_HAntiCheatServer acs = nullptr;
	EOS_NotificationId notifyMsgId = 0;
	EOS_NotificationId notifyActionId = 0;
	EOS_NotificationId notifyAuthId = 0;
	bool sessionActive = false;
	bool sdkUp = false;
	double lastTickAt = -1.0e9;

	struct Client {
		std::string puid;
		std::string ip;
		uint8_t platform = 0;
		bool registered = false;
		bool verifying = false;
	};
	std::map<void*, Client> clients;

	// --- helpers -----------------------------------------------------------
	void Log(const char* fmt, ...)
	{
		char buf[768];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		if(logFn) logFn(std::string(buf));
	}

	const char* ResultName(EOS_EResult r) const
	{
		return ResultToString ? ResultToString(r) : "EOS_EResult?";
	}

	bool Resolve();
	void Unload();
	bool EnsurePlatform();
	void BeginAcSession();
	void EndAcSession();
	void RegisterVerified(void* handle, const std::string& puid, uint8_t platform, const std::string& ip);
	void RegistrationDone(void* handle, int status, const std::string& why);
};

// ---------------------------------------------------------------------------
// Symbol resolution — every name here must be the exported symbol, not the
// struct field it lands in.
// ---------------------------------------------------------------------------
bool Impl::Resolve()
{
	std::vector<std::string> candidates;

	std::string fromEnv = EnvVar("RELCS_EOSSDK");
	if(!fromEnv.empty()) candidates.push_back(fromEnv);

#ifdef _WIN32
	const char* libName = "EOSSDK-Win64-Shipping.dll";
	candidates.push_back(std::string("vendor/eos-sdk/bin/Win64/") + libName);
	candidates.push_back(std::string("../vendor/eos-sdk/bin/Win64/") + libName);
	candidates.push_back(std::string("../../vendor/eos-sdk/bin/Win64/") + libName);
#else
	const char* libName = "libEOSSDK-Linux-Shipping.so";
	candidates.push_back(std::string("vendor/eos-sdk/lib/Linux/") + libName);
	candidates.push_back(std::string("../vendor/eos-sdk/lib/Linux/") + libName);
	candidates.push_back(std::string("./") + libName);
	candidates.push_back(std::string("../") + libName);
#endif
	if(!cfg.sdkDir.empty()){
		candidates.push_back(AbsolutePath(cfg.sdkDir) + "/" + libName);
		candidates.push_back(cfg.sdkDir);
	}
	candidates.push_back(libName);   // let the loader search its own paths

	std::string missing;
	for(const auto& c : candidates){
		lib = LibOpen(c);
		if(!lib) continue;
		libPath = c;
		missing.clear();

#define RELCS_BIND(field, symbol)                                                \
		field = reinterpret_cast<decltype(field)>(LibSym(lib, symbol));           \
		if(!field) missing += std::string(missing.empty() ? "" : ", ") + symbol;

		RELCS_BIND(Initialize, "EOS_Initialize")
		RELCS_BIND(Shutdown, "EOS_Shutdown")
		RELCS_BIND(Platform_Create, "EOS_Platform_Create")
		RELCS_BIND(Platform_Release, "EOS_Platform_Release")
		RELCS_BIND(Platform_Tick, "EOS_Platform_Tick")
		RELCS_BIND(GetConnectInterface, "EOS_Platform_GetConnectInterface")
		RELCS_BIND(GetAntiCheatServerInterface, "EOS_Platform_GetAntiCheatServerInterface")
		RELCS_BIND(Logging_SetCallback, "EOS_Logging_SetCallback")
		RELCS_BIND(Logging_SetLogLevel, "EOS_Logging_SetLogLevel")
		RELCS_BIND(ResultToString, "EOS_EResult_ToString")
		RELCS_BIND(ProductUserId_FromString, "EOS_ProductUserId_FromString")
		RELCS_BIND(ProductUserId_ToString, "EOS_ProductUserId_ToString")
		RELCS_BIND(VerifyIdToken, "EOS_Connect_VerifyIdToken")
		RELCS_BIND(AddNotifyMessageToClient, "EOS_AntiCheatServer_AddNotifyMessageToClient")
		RELCS_BIND(RemoveNotifyMessageToClient, "EOS_AntiCheatServer_RemoveNotifyMessageToClient")
		RELCS_BIND(AddNotifyClientActionRequired, "EOS_AntiCheatServer_AddNotifyClientActionRequired")
		RELCS_BIND(RemoveNotifyClientActionRequired, "EOS_AntiCheatServer_RemoveNotifyClientActionRequired")
		RELCS_BIND(AddNotifyClientAuthStatusChanged, "EOS_AntiCheatServer_AddNotifyClientAuthStatusChanged")
		RELCS_BIND(RemoveNotifyClientAuthStatusChanged, "EOS_AntiCheatServer_RemoveNotifyClientAuthStatusChanged")
		RELCS_BIND(BeginSession, "EOS_AntiCheatServer_BeginSession")
		RELCS_BIND(EndSession, "EOS_AntiCheatServer_EndSession")
		RELCS_BIND(RegisterClientFn, "EOS_AntiCheatServer_RegisterClient")
		RELCS_BIND(UnregisterClientFn, "EOS_AntiCheatServer_UnregisterClient")
		RELCS_BIND(ReceiveMessageFromClient, "EOS_AntiCheatServer_ReceiveMessageFromClient")
#undef RELCS_BIND

		if(missing.empty()) return true;
		detail = "SDK at " + c + " is missing exports: " + missing;
		LibClose(lib);
		lib = nullptr;
		return false;
	}

	detail = std::string("no EOS SDK library found (looked for ") + libName +
	         "; set RELCS_EOSSDK or eac_sdk_dir)";
	return false;
}

void Impl::Unload()
{
	EndAcSession();
	if(platform && Platform_Release){
		Platform_Release(platform);
		platform = nullptr;
	}
	if(sdkUp && Shutdown){
		Shutdown();
		sdkUp = false;
	}
	if(g_impl == this) g_impl = nullptr;
	if(lib){
		LibClose(lib);
		lib = nullptr;
	}
	connect = nullptr;
	acs = nullptr;
	// drop every entry point: a stale dlsym must never outlive the library
	Initialize = nullptr; Shutdown = nullptr; Platform_Create = nullptr;
	Platform_Release = nullptr; Platform_Tick = nullptr;
	GetConnectInterface = nullptr; GetAntiCheatServerInterface = nullptr;
	Logging_SetCallback = nullptr; Logging_SetLogLevel = nullptr;
	ResultToString = nullptr; ProductUserId_FromString = nullptr;
	ProductUserId_ToString = nullptr; VerifyIdToken = nullptr;
	AddNotifyMessageToClient = nullptr; RemoveNotifyMessageToClient = nullptr;
	AddNotifyClientActionRequired = nullptr; RemoveNotifyClientActionRequired = nullptr;
	AddNotifyClientAuthStatusChanged = nullptr; RemoveNotifyClientAuthStatusChanged = nullptr;
	BeginSession = nullptr; EndSession = nullptr;
	RegisterClientFn = nullptr; UnregisterClientFn = nullptr;
	ReceiveMessageFromClient = nullptr;
	libPath.clear();
}

// ---------------------------------------------------------------------------
// EOS callbacks
// ---------------------------------------------------------------------------
namespace {

void EOS_CALL OnEosLog(const EOS_LogMessage* msg)
{
	if(!msg || !msg->Message) return;
	if(msg->Level != EOS_ELogLevel::EOS_LOG_Warning && msg->Level != EOS_ELogLevel::EOS_LOG_Error &&
	   msg->Level != EOS_ELogLevel::EOS_LOG_Fatal)
		return;
	if(!g_impl) return;
	g_impl->Log("[EAC][eos-sdk] %s: %s", msg->Category ? msg->Category : "?", msg->Message);

	// Keep the last anti-cheat complaint visible in `ac status`. The one that
	// matters in practice is a backend 403 ("policy_missing_action": the EOS
	// client lacks anticheat:authenticate*), which BeginSession cannot detect
	// because it only creates the local session.
	std::string text = msg->Message;
	if(text.find("ntiCheat") == std::string::npos && text.find("nticheat") == std::string::npos)
		return;
	std::string e = std::string(msg->Category ? msg->Category : "?") + ": " + text;
	if(e.size() > 240) e = e.substr(0, 237) + "...";
	g_impl->lastError = e;
}

void EOS_CALL OnMessageToClient(const EOS_AntiCheatCommon_OnMessageToClientCallbackInfo* info)
{
	if(!info || !g_impl) return;
	auto it = g_impl->clients.find(info->ClientHandle);
	if(it == g_impl->clients.end() || !it->second.registered){
		g_impl->Log("[EAC] dropping AC message for an unregistered client handle");
		return;
	}
	if(info->MessageDataSizeBytes > (uint32_t)rnet::MAX_AC_PAYLOAD){
		g_impl->Log("[EAC] AC message too large for the relay (%u bytes)", (unsigned)info->MessageDataSizeBytes);
		return;
	}
	if(g_impl->toClientFn) g_impl->toClientFn(info->ClientHandle, info->MessageData, info->MessageDataSizeBytes);
}

void EOS_CALL OnClientActionRequired(const EOS_AntiCheatCommon_OnClientActionRequiredCallbackInfo* info)
{
	if(!info || !g_impl) return;
	const char* reason = info->ActionReasonDetailsString ? info->ActionReasonDetailsString : "anti-cheat violation";
	g_impl->Log("[EAC] action required: action=%d reason=%d (%s)",
	            (int)info->ClientAction, (int)info->ActionReasonCode, reason);
	if(g_impl->actionRequiredFn)
		g_impl->actionRequiredFn(info->ClientHandle, (int)info->ClientAction,
		                         (int)info->ActionReasonCode, reason);
}

void EOS_CALL OnClientAuthStatusChanged(const EOS_AntiCheatCommon_OnClientAuthStatusChangedCallbackInfo* info)
{
	if(!info || !g_impl) return;
	g_impl->Log("[EAC] client auth status %d", (int)info->ClientAuthStatus);
}

struct PendingVerify {
	Impl* impl = nullptr;
	void* handle = nullptr;
	std::string puid;
	std::string ip;
	uint8_t platform = 0;
};

void EOS_CALL OnVerifyIdToken(const EOS_Connect_VerifyIdTokenCallbackInfo* info)
{
	PendingVerify* pend = info ? static_cast<PendingVerify*>(info->ClientData) : nullptr;
	if(!pend) return;

	Impl* impl = pend->impl;
	void* handle = pend->handle;
	std::string claimed = pend->puid;
	std::string ip = pend->ip;
	uint8_t platform = pend->platform;
	struct Guard { PendingVerify* p; ~Guard(){ delete p; } } guard{pend};

	// the peer may have left while the token was being checked
	if(impl->stillConnectedFn && !impl->stillConnectedFn(handle))
		return;

	if(!info || info->ResultCode != EOS_EResult::EOS_Success){
		impl->Log("[EAC] id token rejected: %s", impl->ResultName(info ? info->ResultCode : EOS_EResult::EOS_InvalidParameters));
		impl->RegistrationDone(handle, 4 /* AC_REG_BAD_TOKEN */, "Connect ID token rejected");
		return;
	}

	// The verified identity is authoritative: a client that presents somebody
	// else's token must not be registered under the PUID it claimed.
	std::string verified;
	if(info->ProductUserId && impl->ProductUserId_ToString){
		char buf[64] = {};
		int32_t len = (int32_t)sizeof buf;
		EOS_EResult rc = impl->ProductUserId_ToString(info->ProductUserId, buf, &len);
		// EOS reports the buffer length *including* the terminating NUL (33 for a
		// 32-char PUID). Slicing by that length kept the NUL in the string, so the
		// hex check below rejected every valid token with "carried no ProductUserId".
		if(rc == EOS_EResult::EOS_Success && buf[0]) verified.assign(buf);
	}
	if(!IsProductUserId(verified)){
		impl->RegistrationDone(handle, 3 /* AC_REG_BAD_PUID */, "token verified but carried no ProductUserId");
		return;
	}
	if(!claimed.empty() && _stricmp(claimed.c_str(), verified.c_str()) != 0){
		impl->Log("[EAC] PUID mismatch: claimed %s, token is %s", claimed.c_str(), verified.c_str());
		impl->RegistrationDone(handle, 3 /* AC_REG_BAD_PUID */, "ProductUserId does not match the ID token");
		return;
	}

	impl->RegisterVerified(handle, verified, platform, ip);
}

} // namespace

// ---------------------------------------------------------------------------
// Impl: session lifecycle
// ---------------------------------------------------------------------------
bool Impl::EnsurePlatform()
{
	if(platform) return true;

	// Reaching the service needs a client credential pair even for a dedicated
	// server: EOS_Platform_Create has no anonymous mode.
	if(cfg.clientId.empty() || cfg.clientSecret.empty()){
		detail = "EOS client credentials missing — Dev Portal → Product → Clients → Create Client, "
		         "then put client_id/client_secret in eos.ini beside the server "
		         "(or eac_client_id/eac_client_secret in server.cfg)";
		Log("[EAC] %s", detail.c_str());
		return false;
	}

	if(!sdkUp){
		EOS_InitializeOptions opts = {};
		opts.ApiVersion = EOS_INITIALIZE_API_LATEST;
		opts.ProductName = "reLCS dedicated server";
		opts.ProductVersion = "1.0.0";
		EOS_EResult rc = Initialize(&opts);
		if(rc != EOS_EResult::EOS_Success){
			detail = std::string("EOS_Initialize failed: ") + ResultName(rc);
			Log("[EAC] %s", detail.c_str());
			return false;
		}
		sdkUp = true;
		g_impl = this;
		if(Logging_SetCallback){
			Logging_SetCallback(OnEosLog);
			if(Logging_SetLogLevel)
				Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
		}
	}

	std::string cache = AbsolutePath(cfg.cacheDir.empty() ? std::string("eos-cache") : cfg.cacheDir);
	MakeDir(cache);

	EOS_Platform_Options opts = {};
	opts.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
	opts.ProductId = cfg.productId.c_str();
	opts.SandboxId = cfg.sandboxId.c_str();
	opts.DeploymentId = cfg.deploymentId.c_str();
	opts.ClientCredentials.ClientId = cfg.clientId.c_str();
	opts.ClientCredentials.ClientSecret = cfg.clientSecret.c_str();
	opts.bIsServer = EOS_TRUE;
	opts.CacheDirectory = cache.c_str();
	opts.TickBudgetInMilliseconds = 0;
	opts.Flags = 0;

	platform = Platform_Create(&opts);
	if(!platform){
		detail = "EOS_Platform_Create failed (client credentials not accepted, or product/sandbox/deployment mismatch)";
		Log("[EAC] %s", detail.c_str());
		return false;
	}

	connect = GetConnectInterface(platform);
	acs = GetAntiCheatServerInterface(platform);
	if(!acs){
		detail = "EOS platform has no AntiCheatServer interface (is anti-cheat enabled for the product?)";
		Log("[EAC] %s", detail.c_str());
		return false;
	}
	Log("[EAC] platform ready (product=%s sandbox=%s deployment=%s, cache=%s)",
	    cfg.productId.c_str(), cfg.sandboxId.c_str(), cfg.deploymentId.c_str(), cache.c_str());
	return true;
}

void Impl::BeginAcSession()
{
	if(!EnsurePlatform()){
		if(state == State::Off && platform == nullptr && !sdkUp) state = State::NotConfigured;
		return;
	}

	{
		EOS_AntiCheatServer_AddNotifyMessageToClientOptions o = {};
		o.ApiVersion = EOS_ANTICHEATSERVER_ADDNOTIFYMESSAGETOCLIENT_API_LATEST;
		notifyMsgId = AddNotifyMessageToClient(acs, &o, this, OnMessageToClient);
	}
	{
		EOS_AntiCheatServer_AddNotifyClientActionRequiredOptions o = {};
		o.ApiVersion = EOS_ANTICHEATSERVER_ADDNOTIFYCLIENTACTIONREQUIRED_API_LATEST;
		notifyActionId = AddNotifyClientActionRequired(acs, &o, this, OnClientActionRequired);
	}
	{
		EOS_AntiCheatServer_AddNotifyClientAuthStatusChangedOptions o = {};
		o.ApiVersion = EOS_ANTICHEATSERVER_ADDNOTIFYCLIENTAUTHSTATUSCHANGED_API_LATEST;
		notifyAuthId = AddNotifyClientAuthStatusChanged(acs, &o, this, OnClientAuthStatusChanged);
	}

	EOS_AntiCheatServer_BeginSessionOptions o = {};
	o.ApiVersion = EOS_ANTICHEATSERVER_BEGINSESSION_API_LATEST;
	o.RegisterTimeoutSeconds = cfg.registerTimeout;
	o.ServerName = cfg.serverName.c_str();
	o.bEnableGameplayData = cfg.enableGameplayData ? EOS_TRUE : EOS_FALSE;
	o.LocalUserId = nullptr;   // dedicated server: no local player

	EOS_EResult rc = BeginSession(acs, &o);
	if(rc != EOS_EResult::EOS_Success){
		detail = std::string("EOS_AntiCheatServer_BeginSession failed: ") + ResultName(rc);
		Log("[EAC] %s", detail.c_str());
		state = State::InitFailed;
		return;
	}
	sessionActive = true;
	state = State::Running;
	detail.clear();
	lastError.clear();
	Log("[EAC] anti-cheat session started (server='%s', register timeout %us)",
	    cfg.serverName.c_str(), (unsigned)cfg.registerTimeout);
}

void Impl::EndAcSession()
{
	if(acs){
		for(auto& kv : clients){
			if(kv.second.registered && UnregisterClientFn){
				EOS_AntiCheatServer_UnregisterClientOptions o = {};
				o.ApiVersion = EOS_ANTICHEATSERVER_UNREGISTERCLIENT_API_LATEST;
				o.ClientHandle = kv.first;
				UnregisterClientFn(acs, &o);
			}
		}
		clients.clear();

		if(sessionActive && EndSession){
			EOS_AntiCheatServer_EndSessionOptions o = {};
			o.ApiVersion = EOS_ANTICHEATSERVER_ENDSESSION_API_LATEST;
			EndSession(acs, &o);
			sessionActive = false;
			Log("[EAC] anti-cheat session ended");
		}
		if(notifyMsgId && RemoveNotifyMessageToClient){
			RemoveNotifyMessageToClient(acs, notifyMsgId);
			notifyMsgId = 0;
		}
		if(notifyActionId && RemoveNotifyClientActionRequired){
			RemoveNotifyClientActionRequired(acs, notifyActionId);
			notifyActionId = 0;
		}
		if(notifyAuthId && RemoveNotifyClientAuthStatusChanged){
			RemoveNotifyClientAuthStatusChanged(acs, notifyAuthId);
			notifyAuthId = 0;
		}
	}
	if(state == State::Running) state = State::Off;
}

// ---------------------------------------------------------------------------
// Impl: registration
// ---------------------------------------------------------------------------
void Impl::RegisterVerified(void* handle, const std::string& puid, uint8_t platformId, const std::string& ip)
{
	if(!acs || !sessionActive || !RegisterClientFn){
		RegistrationDone(handle, 1 /* AC_REG_DISABLED */, "no anti-cheat session");
		return;
	}

	EOS_AntiCheatServer_RegisterClientOptions o = {};
	o.ApiVersion = EOS_ANTICHEATSERVER_REGISTERCLIENT_API_LATEST;
	o.ClientHandle = handle;
	// A peer that runs a local EOS AC session is a protected client; anything
	// else is registered as unprotected so the session still tracks it.
	bool protectedClient = platformId == 1 || platformId == 2 || platformId == 3;
	o.ClientType = protectedClient ? EOS_EAntiCheatCommonClientType::EOS_ACCCT_ProtectedClient
	                               : EOS_EAntiCheatCommonClientType::EOS_ACCCT_UnprotectedClient;
	o.ClientPlatform = (EOS_EAntiCheatCommonClientPlatform)platformId;
	o.IpAddress = ip.empty() ? nullptr : ip.c_str();
	o.UserId = ProductUserId_FromString ? ProductUserId_FromString(puid.c_str()) : nullptr;
	if(!o.UserId){
		RegistrationDone(handle, 3 /* AC_REG_BAD_PUID */, "ProductUserId could not be parsed");
		return;
	}

	EOS_EResult rc = RegisterClientFn(acs, &o);
	if(rc != EOS_EResult::EOS_Success){
		RegistrationDone(handle, 5 /* AC_REG_REJECTED */, ResultName(rc));
		return;
	}

	Client& c = clients[handle];
	c.puid = puid;
	c.ip = ip;
	c.platform = platformId;
	c.registered = true;
	c.verifying = false;
	Log("[EAC] registered client %s (%s, %s)", puid.c_str(), PlatformName(platformId), ip.c_str());
	if(registerResultFn) registerResultFn(handle, 0 /* AC_REG_OK */, std::string());
}

void Impl::RegistrationDone(void* handle, int status, const std::string& why)
{
	auto it = clients.find(handle);
	if(it != clients.end()){
		it->second.verifying = false;
		if(status != 0) clients.erase(it);
	}
	Log("[EAC] registration refused (%d): %s", status, why.c_str());
	if(registerResultFn) registerResultFn(handle, status, why);
}

// ---------------------------------------------------------------------------
// Server facade
// ---------------------------------------------------------------------------
Server::Server() : m_impl(new Impl()) {}
Server::~Server() { Stop(); }

void Server::SetLog(LogFn fn) { m_impl->logFn = std::move(fn); }

void Server::SetCallbacks(MessageToClientFn toClient, ActionRequiredFn actionRequired,
                          StillConnectedFn stillConnected, RegisterResultFn registerResult)
{
	m_impl->toClientFn = std::move(toClient);
	m_impl->actionRequiredFn = std::move(actionRequired);
	m_impl->stillConnectedFn = std::move(stillConnected);
	m_impl->registerResultFn = std::move(registerResult);
}

bool Server::Start(const Config& cfg)
{
	Stop();
	m_impl->cfg = cfg;

	if(!cfg.enabled){
		m_impl->state = State::Off;
		m_impl->detail = "disabled by config (eac=0)";
		m_impl->Log("[EAC] %s", m_impl->detail.c_str());
		return false;
	}

	Resolve(m_impl->cfg);   // ids from Settings.json, credentials from eos.ini
	m_impl->lastError.clear();

	if(!m_impl->Resolve()){
		m_impl->state = State::SdkMissing;
		m_impl->Log("[EAC] DISABLED: %s", m_impl->detail.c_str());
		return false;
	}
	m_impl->Log("[EAC] SDK %s loaded from %s", m_impl->sdkVersion.c_str(), m_impl->libPath.c_str());

	m_impl->BeginAcSession();
	if(m_impl->state != State::Running){
		if(m_impl->state == State::Off) m_impl->state = State::InitFailed;
		m_impl->Log("[EAC] DISABLED: %s", m_impl->detail.c_str());
		m_impl->Unload();
		return false;
	}
	return true;
}

void Server::Stop()
{
	m_impl->EndAcSession();
	m_impl->Unload();
	if(m_impl->state == State::Running) m_impl->state = State::Off;
}

void Server::Tick(double nowSeconds)
{
	if(!m_impl->platform || !m_impl->Platform_Tick) return;
	m_impl->lastTickAt = nowSeconds;
	m_impl->Platform_Tick(m_impl->platform);
}

bool Server::TickDue(double nowSeconds) const
{
	if(!m_impl->platform) return false;
	// 20 Hz is plenty: the session only needs its heartbeat and queues pumped.
	return nowSeconds - m_impl->lastTickAt >= 0.05;
}

bool Server::Ready() const { return m_impl->state == State::Running && m_impl->sessionActive; }
State Server::state() const { return m_impl->state; }
const std::string& Server::detail() const { return m_impl->detail; }
const std::string& Server::sdkVersion() const { return m_impl->sdkVersion; }
const Config& Server::config() const { return m_impl->cfg; }

int Server::RegisterClient(void* handle, const std::string& productUserId,
                           const std::string& idToken, uint8_t platform, const std::string& ip)
{
	if(!Ready()) return 1;                                          // AC_REG_DISABLED
	if(platform != 1 && platform != 2 && platform != 3) return 2;   // AC_REG_BAD_PLATFORM
	if(!IsProductUserId(productUserId)) return 3;                   // AC_REG_BAD_PUID
	if(!handle) return 5;

	auto it = m_impl->clients.find(handle);
	if(it != m_impl->clients.end() && (it->second.registered || it->second.verifying))
		return 6;   // AC_REG_ALREADY

	Impl::Client& c = m_impl->clients[handle];
	c.puid = productUserId;
	c.ip = ip;
	c.platform = platform;

	if(!m_impl->cfg.requireToken){
		m_impl->RegisterVerified(handle, productUserId, platform, ip);
		return 0;
	}

	if(idToken.empty() || !m_impl->connect || !m_impl->VerifyIdToken){
		m_impl->clients.erase(handle);
		return 4;   // AC_REG_BAD_TOKEN
	}
	c.verifying = true;

	auto* pend = new PendingVerify();
	pend->impl = m_impl.get();
	pend->handle = handle;
	pend->puid = productUserId;
	pend->ip = ip;
	pend->platform = platform;

	EOS_Connect_IdToken token = {};
	token.ApiVersion = EOS_CONNECT_IDTOKEN_API_LATEST;
	token.ProductUserId = m_impl->ProductUserId_FromString(productUserId.c_str());
	token.JsonWebToken = idToken.c_str();

	EOS_Connect_VerifyIdTokenOptions o = {};
	o.ApiVersion = EOS_CONNECT_VERIFYIDTOKEN_API_LATEST;
	o.IdToken = &token;

	m_impl->VerifyIdToken(m_impl->connect, &o, pend, OnVerifyIdToken);
	return 0;   // verification in flight
}

void Server::UnregisterClient(void* handle)
{
	if(!handle) return;
	auto it = m_impl->clients.find(handle);
	if(it == m_impl->clients.end()) return;

	if(it->second.registered && m_impl->acs && m_impl->UnregisterClientFn){
		EOS_AntiCheatServer_UnregisterClientOptions o = {};
		o.ApiVersion = EOS_ANTICHEATSERVER_UNREGISTERCLIENT_API_LATEST;
		o.ClientHandle = handle;
		m_impl->UnregisterClientFn(m_impl->acs, &o);
	}
	if(!it->second.puid.empty())
		m_impl->Log("[EAC] unregistered client %s", it->second.puid.c_str());
	m_impl->clients.erase(it);
}

bool Server::IsRegistered(void* handle) const
{
	auto it = m_impl->clients.find(handle);
	return it != m_impl->clients.end() && it->second.registered;
}

int Server::ClientCount() const
{
	int n = 0;
	for(const auto& kv : m_impl->clients)
		if(kv.second.registered) n++;
	return n;
}

void Server::ReceiveFromClient(void* handle, const void* data, uint32_t len)
{
	if(!Ready() || !m_impl->acs || !m_impl->ReceiveMessageFromClient || !data || !len) return;
	if(!IsRegistered(handle)) return;
	if(len > (uint32_t)rnet::MAX_AC_PAYLOAD){
		m_impl->Log("[EAC] dropping oversized AC payload from a client (%u bytes)", (unsigned)len);
		return;
	}

	EOS_AntiCheatServer_ReceiveMessageFromClientOptions o = {};
	o.ApiVersion = EOS_ANTICHEATSERVER_RECEIVEMESSAGEFROMCLIENT_API_LATEST;
	o.ClientHandle = handle;
	o.Data = data;
	o.DataLengthBytes = len;
	EOS_EResult rc = m_impl->ReceiveMessageFromClient(m_impl->acs, &o);
	if(rc != EOS_EResult::EOS_Success)
		m_impl->Log("[EAC] ReceiveMessageFromClient failed: %s", m_impl->ResultName(rc));
}

std::string Server::StatusLine() const
{
	char buf[512];
	snprintf(buf, sizeof buf, "eac=%s state=%s clients=%d pending=%d",
	         m_impl->cfg.enabled ? "on" : "off", StateName(m_impl->state),
	         ClientCount(), (int)m_impl->clients.size());
	std::string s(buf);
	if(!m_impl->libPath.empty()) s += " lib=" + m_impl->libPath;
	if(!m_impl->detail.empty()) s += " detail=\"" + m_impl->detail + "\"";
	if(!m_impl->lastError.empty()) s += " lastError=\"" + m_impl->lastError + "\"";
	return s;
}

std::vector<Server::ClientInfo> Server::Clients() const
{
	std::vector<ClientInfo> out;
	for(const auto& kv : m_impl->clients){
		ClientInfo ci;
		ci.handle = kv.first;
		ci.productUserId = kv.second.puid;
		ci.ip = kv.second.ip;
		ci.platform = kv.second.platform;
		ci.registered = kv.second.registered;
		ci.verifying = kv.second.verifying;
		out.push_back(ci);
	}
	return out;
}

} // namespace ac
