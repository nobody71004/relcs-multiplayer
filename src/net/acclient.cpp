// src/net/acclient.cpp — EOS Anti-Cheat client session (see acclient.h).
//
// Flow: load EOSSDK at runtime -> EOS_Initialize -> EOS_Platform_Create (client)
// -> GetAntiCheatClientInterface -> Connect device-id login -> CopyIdToken ->
// BeginSession(ClientServer). From then on the kernel driver's messages are
// handed to NetClient (MSG_AC_FROM_CLIENT) and the server's replies are pushed
// back in (EOS_AntiCheatClient_ReceiveMessageFromServer).
//
// Every EOS call runs on the game's main thread (Tick); the SDK's callbacks are
// issued from inside EOS_Platform_Tick, so the state below needs no locking.
#include "acclient.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>

#ifdef _WIN32

#include <windows.h>

#include <eos_sdk.h>
#include <eos_anticheatclient.h>
#include <eos_connect.h>

#include "rnet_eos.h"

namespace acclient {
namespace {

using Clock = std::chrono::steady_clock;

double NowSec()
{
	return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Runtime binding: the game must start even when the SDK is not installed, so
// nothing here is linked. decltype() keeps every pointer's real signature
// (including EOS_CALL) without needing the import library.
// ---------------------------------------------------------------------------
struct Sdk {
	HMODULE module = nullptr;
	decltype(&EOS_Initialize) Initialize = nullptr;
	decltype(&EOS_Shutdown) Shutdown = nullptr;
	decltype(&EOS_Platform_Create) PlatformCreate = nullptr;
	decltype(&EOS_Platform_Release) PlatformRelease = nullptr;
	decltype(&EOS_Platform_Tick) PlatformTick = nullptr;
	decltype(&EOS_Platform_GetConnectInterface) GetConnect = nullptr;
	decltype(&EOS_Platform_GetAntiCheatClientInterface) GetAntiCheatClient = nullptr;
	decltype(&EOS_Connect_CreateDeviceId) CreateDeviceId = nullptr;
	decltype(&EOS_Connect_Login) Login = nullptr;
	decltype(&EOS_Connect_CopyIdToken) CopyIdToken = nullptr;
	decltype(&EOS_Connect_IdToken_Release) IdTokenRelease = nullptr;
	decltype(&EOS_ProductUserId_ToString) PuidToString = nullptr;
	decltype(&EOS_AntiCheatClient_BeginSession) BeginSession = nullptr;
	decltype(&EOS_AntiCheatClient_EndSession) EndSession = nullptr;
	decltype(&EOS_AntiCheatClient_AddNotifyMessageToServer) AddNotifyToServer = nullptr;
	decltype(&EOS_AntiCheatClient_RemoveNotifyMessageToServer) RemoveNotifyToServer = nullptr;
	decltype(&EOS_AntiCheatClient_AddNotifyClientIntegrityViolated) AddNotifyViolation = nullptr;
	decltype(&EOS_AntiCheatClient_RemoveNotifyClientIntegrityViolated) RemoveNotifyViolation = nullptr;
	decltype(&EOS_AntiCheatClient_ReceiveMessageFromServer) ReceiveFromServer = nullptr;
};

struct Ctx {
	Sdk sdk;

	bool started = false;
	bool devicePending = false;
	bool loginPending = false;
	bool retryPending = false;
	State state = State::Off;
	std::string detail;
	std::string violation;

	EOS_HPlatform platform = nullptr;
	EOS_HConnect connect = nullptr;
	EOS_HAntiCheatClient ac = nullptr;
	EOS_ProductUserId puid = nullptr;
	char puidText[64] = {};

	EOS_NotificationId notifyToServer = 0;
	EOS_NotificationId notifyViolation = 0;

	ToServerFn toServer = nullptr;

	// async results: -2 = in flight, -1 = not sent, else EOS_EResult
	int deviceResult = -1;
	int loginResult = -1;
	double stepAt = 0.0;
	int beginAttempts = 0;
};

Ctx g;
FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
	if(!g_log){
		g_log = std::fopen("eos-ac.log", "w");
		if(!g_log) return;
		std::fprintf(g_log, "reLCS EOS anti-cheat client\n");
	}
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	std::fprintf(g_log, "%s\n", buf);
	std::fflush(g_log);
}

const char* ResultName(int r)
{
	switch(r){
	case (int)EOS_EResult::EOS_Success: return "success";
	case (int)EOS_EResult::EOS_DuplicateNotAllowed: return "already-exists";
	case (int)EOS_EResult::EOS_InvalidParameters: return "invalid-parameters";
	case (int)EOS_EResult::EOS_InvalidUser: return "invalid-user";
	case (int)EOS_EResult::EOS_NotConfigured: return "not-configured";
	case (int)EOS_EResult::EOS_NoConnection: return "no-connection";
	case (int)EOS_EResult::EOS_TimedOut: return "timed-out";
	case (int)EOS_EResult::EOS_ServiceFailure: return "service-failure";
	case (int)EOS_EResult::EOS_UnexpectedError: return "unexpected-error";
	default: return "eos-error";
	}
}

void Fail(const std::string& why)
{
	g.state = State::Failed;
	g.detail = why;
	g.devicePending = g.loginPending = g.retryPending = false;
	Log("[ac] failed: %s", why.c_str());
}

std::string ExeDir()
{
	char exe[MAX_PATH] = {};
	DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string dir;
	if(n > 0 && n < MAX_PATH){
		dir.assign(exe, n);
		size_t s = dir.find_last_of("\\/");
		if(s != std::string::npos) dir.resize(s);
	}
	return dir;
}

// The SDK lives next to the game (the launcher puts it there); a dev tree keeps
// it in vendor/eos-sdk/bin. The exe directory is tried before the bare name so
// an unrelated copy in the working directory can never win.
HMODULE LoadSdk(const std::string& exeDir)
{
	std::string local = exeDir.empty() ? std::string() : exeDir + "\\EOSSDK-Win64-Shipping.dll";
	const std::string names[] = { local, "EOSSDK-Win64-Shipping.dll" };
	for(const std::string& n : names){
		if(n.empty()) continue;
		HMODULE m = LoadLibraryA(n.c_str());
		if(m){
			Log("[ac] SDK loaded: %s", n.c_str());
			return m;
		}
	}
	std::string dev = exeDir + "\\..\\..\\..\\..\\vendor\\eos-sdk\\bin\\Win64\\EOSSDK-Win64-Shipping.dll";
	HMODULE m = LoadLibraryA(dev.c_str());
	if(m) Log("[ac] SDK loaded (dev tree): %s", dev.c_str());
	return m;
}

template<typename T>
bool Bind(HMODULE mod, const char* name, T& out)
{
	out = (T)GetProcAddress(mod, name);
	if(!out) Log("[ac] missing export: %s", name);
	return out != nullptr;
}

// client_id / client_secret: eos.ini next to the game, else next to the
// launcher (the launcher copies it into the game folder on join).
bool ReadCredential(const std::string& path, const char* key, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if(!f) return false;
	char line[512];
	bool found = false;
	while(std::fgets(line, sizeof line, f)){
		std::string s(line);
		size_t a = s.find_first_not_of(" \t");
		if(a == std::string::npos) continue;
		s = s.substr(a);
		if(s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;
		size_t eq = s.find('=');
		if(eq == std::string::npos) continue;
		std::string k = s.substr(0, eq);
		while(!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
		if(k != key) continue;
		std::string v = s.substr(eq + 1);
		size_t e = v.find_last_not_of(" \t\r\n");
		v = (e == std::string::npos) ? std::string() : v.substr(0, e + 1);
		// a trailing "  # comment" is not part of the value
		size_t hash = v.find(" #");
		if(hash != std::string::npos) v = v.substr(0, hash);
		out = v;
		found = !v.empty();
		break;
	}
	std::fclose(f);
	return found;
}

bool LoadCredentials(const std::string& exeDir, std::string& id, std::string& secret)
{
	const std::string paths[] = {
		exeDir + "\\eos.ini",
		"eos.ini",
		exeDir + "\\..\\launcher\\eos.ini",
		exeDir + "\\..\\..\\launcher\\eos.ini",
	};
	for(const std::string& p : paths){
		if(ReadCredential(p, "client_id", id) && ReadCredential(p, "client_secret", secret)){
			Log("[ac] client credentials from %s", p.c_str());
			return true;
		}
		id.clear();
		secret.clear();
	}
	return false;
}

// ---------------------------------------------------------------------------
// EOS callbacks (all issued from inside EOS_Platform_Tick)
// ---------------------------------------------------------------------------
void EOS_CALL OnDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo* d)
{
	g.deviceResult = (int)d->ResultCode;
	Log("[ac] CreateDeviceId -> %s", ResultName(g.deviceResult));
}

void EOS_CALL OnLogin(const EOS_Connect_LoginCallbackInfo* d)
{
	if(d->ResultCode == EOS_EResult::EOS_Success && d->LocalUserId){
		g.puid = d->LocalUserId;
		int32_t len = (int32_t)sizeof g.puidText;
		if(g.sdk.PuidToString && g.sdk.PuidToString(g.puid, g.puidText, &len) != EOS_EResult::EOS_Success)
			g.puidText[0] = 0;
		g.loginResult = 0;
	}else{
		g.loginResult = (int)d->ResultCode;
	}
	Log("[ac] Connect login -> %s (%s)", ResultName(g.loginResult), g.puidText);
}

void EOS_CALL OnMessageToServer(const EOS_AntiCheatClient_OnMessageToServerCallbackInfo* d)
{
	if(!g.toServer || !d->MessageData || d->MessageDataSizeBytes == 0) return;
	g.toServer(d->MessageData, d->MessageDataSizeBytes);
}

void EOS_CALL OnIntegrityViolated(const EOS_AntiCheatClient_OnClientIntegrityViolatedCallbackInfo* d)
{
	const char* msg = (d->ViolationMessage && *d->ViolationMessage)
	    ? d->ViolationMessage : "anti-cheat integrity check failed";
	g.violation = msg;
	Log("[ac] integrity violation (type %d): %s", (int)d->ViolationType, msg);
}

// ---------------------------------------------------------------------------
bool BindSdk()
{
	std::string exeDir = ExeDir();
	g.sdk.module = LoadSdk(exeDir);
	if(!g.sdk.module){
		g.state = State::SdkMissing;
		g.detail = "EOSSDK-Win64-Shipping.dll is not next to the game — "
		           "the launcher installs it on join";
		return false;
	}
	Sdk& s = g.sdk;
	HMODULE m = s.module;
	bool ok = true;
	ok &= Bind(m, "EOS_Initialize", s.Initialize);
	ok &= Bind(m, "EOS_Shutdown", s.Shutdown);
	ok &= Bind(m, "EOS_Platform_Create", s.PlatformCreate);
	ok &= Bind(m, "EOS_Platform_Release", s.PlatformRelease);
	ok &= Bind(m, "EOS_Platform_Tick", s.PlatformTick);
	ok &= Bind(m, "EOS_Platform_GetConnectInterface", s.GetConnect);
	ok &= Bind(m, "EOS_Platform_GetAntiCheatClientInterface", s.GetAntiCheatClient);
	ok &= Bind(m, "EOS_Connect_CreateDeviceId", s.CreateDeviceId);
	ok &= Bind(m, "EOS_Connect_Login", s.Login);
	ok &= Bind(m, "EOS_Connect_CopyIdToken", s.CopyIdToken);
	ok &= Bind(m, "EOS_Connect_IdToken_Release", s.IdTokenRelease);
	ok &= Bind(m, "EOS_ProductUserId_ToString", s.PuidToString);
	ok &= Bind(m, "EOS_AntiCheatClient_BeginSession", s.BeginSession);
	ok &= Bind(m, "EOS_AntiCheatClient_EndSession", s.EndSession);
	ok &= Bind(m, "EOS_AntiCheatClient_AddNotifyMessageToServer", s.AddNotifyToServer);
	ok &= Bind(m, "EOS_AntiCheatClient_RemoveNotifyMessageToServer", s.RemoveNotifyToServer);
	ok &= Bind(m, "EOS_AntiCheatClient_AddNotifyClientIntegrityViolated", s.AddNotifyViolation);
	ok &= Bind(m, "EOS_AntiCheatClient_RemoveNotifyClientIntegrityViolated", s.RemoveNotifyViolation);
	ok &= Bind(m, "EOS_AntiCheatClient_ReceiveMessageFromServer", s.ReceiveFromServer);
	if(!ok){
		g.state = State::InitFailed;
		g.detail = "this EOSSDK is too old for anti-cheat (1.19 or newer is required)";
		return false;
	}
	return true;
}

bool CreatePlatform()
{
	EOS_InitializeOptions init = {};
	init.ApiVersion = EOS_INITIALIZE_API_LATEST;
	init.ProductName = rnet::EOS_GAME_TITLE;
	init.ProductVersion = "1.0.0";
	EOS_EResult rc = g.sdk.Initialize(&init);
	if(rc != EOS_EResult::EOS_Success){
		Fail(std::string("EOS_Initialize failed: ") + ResultName((int)rc));
		return false;
	}

	// EOS 1.19 refuses a relative cache path
	std::string cache = ExeDir() + "\\eos-ac-cache";
	CreateDirectoryA(cache.c_str(), nullptr);

	std::string clientId, clientSecret;
	bool haveCreds = LoadCredentials(ExeDir(), clientId, clientSecret);

	EOS_Platform_Options opts = {};
	opts.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
	opts.ProductId = rnet::EOS_PRODUCT_ID;
	opts.SandboxId = rnet::EOS_SANDBOX_ID;
	opts.DeploymentId = rnet::EOS_DEPLOYMENT_ID;
	opts.bIsServer = EOS_FALSE;
	opts.CacheDirectory = cache.c_str();
	opts.Flags = EOS_PF_DISABLE_OVERLAY;
	if(haveCreds){
		opts.ClientCredentials.ClientId = clientId.c_str();
		opts.ClientCredentials.ClientSecret = clientSecret.c_str();
	}
	g.platform = g.sdk.PlatformCreate(&opts);
	if(!g.platform && haveCreds){
		// a rejected credential pair must not cost the player the session:
		// retry unauthenticated (device-id auth does not need them in 1.19)
		Log("[ac] platform create with client credentials failed, retrying without");
		opts.ClientCredentials.ClientId = nullptr;
		opts.ClientCredentials.ClientSecret = nullptr;
		g.platform = g.sdk.PlatformCreate(&opts);
	}
	if(!g.platform){
		Fail("EOS_Platform_Create failed (product/sandbox/deployment mismatch?)");
		return false;
	}

	g.connect = g.sdk.GetConnect(g.platform);
	g.ac = g.sdk.GetAntiCheatClient(g.platform);
	if(!g.connect || !g.ac){
		Fail("EOS anti-cheat interface unavailable (enable Anti-Cheat for the product)");
		return false;
	}

	EOS_AntiCheatClient_AddNotifyMessageToServerOptions n1 = {};
	n1.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYMESSAGETOSERVER_API_LATEST;
	g.notifyToServer = g.sdk.AddNotifyToServer(g.ac, &n1, nullptr, OnMessageToServer);

	EOS_AntiCheatClient_AddNotifyClientIntegrityViolatedOptions n2 = {};
	n2.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYCLIENTINTEGRITYVIOLATED_API_LATEST;
	g.notifyViolation = g.sdk.AddNotifyViolation(g.ac, &n2, nullptr, OnIntegrityViolated);
	return true;
}

void BeginSession()
{
	EOS_AntiCheatClient_BeginSessionOptions opts = {};
	opts.ApiVersion = EOS_ANTICHEATCLIENT_BEGINSESSION_API_LATEST;
	opts.LocalUserId = g.puid;
	opts.Mode = EOS_EAntiCheatClientMode::EOS_ACCM_ClientServer;
	EOS_EResult rc = g.sdk.BeginSession(g.ac, &opts);
	if(rc == EOS_EResult::EOS_Success){
		g.state = State::Ready;
		g.detail.clear();
		Log("[ac] session live as %s", g.puidText);
		return;
	}
	// the anti-cheat service is sometimes still starting right after login
	if(++g.beginAttempts < 3){
		Log("[ac] BeginSession -> %s (retrying)", ResultName((int)rc));
		g.retryPending = true;
		g.stepAt = NowSec() + 3.0;
		g.detail = std::string("starting anti-cheat: ") + ResultName((int)rc);
		return;
	}
	Fail(std::string("EOS_AntiCheatClient_BeginSession failed: ") + ResultName((int)rc) +
	     " (the Easy Anti-Cheat service must be installed and running)");
}

void SendDeviceId()
{
	EOS_Connect_CreateDeviceIdOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
	opts.DeviceModel = "PC Windows";
	g.deviceResult = -2;
	g.devicePending = true;
	g.stepAt = NowSec();
	g.sdk.CreateDeviceId(g.connect, &opts, nullptr, OnDeviceId);
	Log("[ac] CreateDeviceId requested");
}

void SendLogin()
{
	EOS_Connect_Credentials creds = {};
	creds.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
	creds.Token = "";
	creds.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;

	EOS_Connect_UserLoginInfo info = {};
	info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
	info.DisplayName = "reLCS Player";

	EOS_Connect_LoginOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
	opts.Credentials = &creds;
	opts.UserLoginInfo = &info;

	g.loginResult = -2;
	g.loginPending = true;
	g.stepAt = NowSec();
	g.sdk.Login(g.connect, &opts, nullptr, OnLogin);
}

} // namespace

// ---------------------------------------------------------------------------
void SetSendToServer(ToServerFn fn) { g.toServer = fn; }

bool Init()
{
	if(g.state == State::Ready) return true;
	if(g.started && g.state != State::Failed) return false;
	if(g.state == State::Failed){
		// a failed session can be retried by the next join
		g.state = State::Off;
		g.started = false;
	}
	g.started = true;
	g.devicePending = g.loginPending = g.retryPending = false;
	g.violation.clear();
	g.puid = nullptr;
	g.puidText[0] = 0;
	g.deviceResult = -1;
	g.loginResult = -1;
	g.beginAttempts = 0;

	if(!BindSdk()) return false;
	if(!CreatePlatform()){
		g.sdk.Shutdown();
		return false;
	}
	g.state = State::LoggingIn;
	g.detail = "starting EOS anti-cheat";
	SendDeviceId();
	return true;
}

void Tick()
{
	if(!g.sdk.PlatformTick || !g.platform) return;
	g.sdk.PlatformTick(g.platform);

	const double now = NowSec();

	if(g.retryPending){
		if(now >= g.stepAt){
			g.retryPending = false;
			BeginSession();
		}
		return;
	}
	if(g.state != State::LoggingIn) return;

	if(g.loginPending){
		if(g.loginResult == -2){
			if(now - g.stepAt > 20.0) Fail("EOS Connect login timed out");
			return;
		}
		g.loginPending = false;
		if(g.loginResult == 0 && g.puid){
			BeginSession();
		}else{
			int lr = g.loginResult;
			Fail(std::string("EOS Connect login refused: ") + ResultName(lr));
		}
		return;
	}

	if(g.devicePending){
		if(g.deviceResult == -2){
			if(now - g.stepAt > 60.0) Fail("EOS device id timed out");
			return;
		}
		g.devicePending = false;
		if(g.deviceResult == (int)EOS_EResult::EOS_Success ||
		   g.deviceResult == (int)EOS_EResult::EOS_DuplicateNotAllowed){
			SendLogin();
		}else{
			Fail(std::string("EOS device id refused: ") + ResultName(g.deviceResult) +
			     " (Connect must be enabled for the client policy)");
		}
	}
}

void Shutdown()
{
	if(!g.sdk.module) return;
	if(g.ac && g.state == State::Ready){
		EOS_AntiCheatClient_EndSessionOptions opts = {};
		opts.ApiVersion = EOS_ANTICHEATCLIENT_ENDSESSION_API_LATEST;
		g.sdk.EndSession(g.ac, &opts);
	}
	if(g.ac){
		if(g.notifyToServer) g.sdk.RemoveNotifyToServer(g.ac, g.notifyToServer);
		if(g.notifyViolation) g.sdk.RemoveNotifyViolation(g.ac, g.notifyViolation);
	}
	g.notifyToServer = g.notifyViolation = 0;
	if(g.platform){
		g.sdk.PlatformRelease(g.platform);
		g.platform = nullptr;
	}
	g.sdk.Shutdown();
	g.ac = nullptr;
	g.connect = nullptr;
	g.state = State::Off;
	g.started = false;
	if(g_log){
		std::fclose(g_log);
		g_log = nullptr;
	}
}

void OnServerMessage(const void* data, uint32_t len)
{
	if(!g.ac || !data || len == 0) return;
	EOS_AntiCheatClient_ReceiveMessageFromServerOptions opts = {};
	opts.ApiVersion = EOS_ANTICHEATCLIENT_RECEIVEMESSAGEFROMSERVER_API_LATEST;
	opts.Data = data;
	opts.DataLengthBytes = len;
	EOS_EResult rc = g.sdk.ReceiveFromServer(g.ac, &opts);
	if(rc != EOS_EResult::EOS_Success)
		Log("[ac] ReceiveMessageFromServer -> %s", ResultName((int)rc));
}

bool Ready() { return g.state == State::Ready; }
State state() { return g.state; }

const char* StateName()
{
	switch(g.state){
	case State::Off: return "off";
	case State::SdkMissing: return "sdk-missing";
	case State::InitFailed: return "init-failed";
	case State::LoggingIn: return "logging-in";
	case State::Failed: return "failed";
	case State::Ready: return "ready";
	}
	return "off";
}

const char* Detail() { return g.detail.c_str(); }
const char* ProductUserId() { return g.puidText; }

bool CopyIdToken(std::string& out)
{
	out.clear();
	if(!g.sdk.CopyIdToken || !g.connect || !g.puid) return false;
	EOS_Connect_CopyIdTokenOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_COPYIDTOKEN_API_LATEST;
	opts.LocalUserId = g.puid;
	EOS_Connect_IdToken* token = nullptr;
	EOS_EResult rc = g.sdk.CopyIdToken(g.connect, &opts, &token);
	if(rc != EOS_EResult::EOS_Success || !token){
		Log("[ac] CopyIdToken -> %s", ResultName((int)rc));
		return false;
	}
	if(token->JsonWebToken) out = token->JsonWebToken;
	g.sdk.IdTokenRelease(token);
	return !out.empty();
}

const char* Violation() { return g.violation.c_str(); }
void ClearViolation() { g.violation.clear(); }

} // namespace acclient

#else // !_WIN32 ------------------------------------------------------------

// The dedicated server and the tests never run an AC client session.
namespace acclient {
namespace {
const char* kMissing = "EOS anti-cheat client is Windows-only";
}

void SetSendToServer(ToServerFn) {}
bool Init() { return false; }
void Tick() {}
void Shutdown() {}
void OnServerMessage(const void*, uint32_t) {}
bool Ready() { return false; }
State state() { return State::SdkMissing; }
const char* StateName() { return "sdk-missing"; }
const char* Detail() { return kMissing; }
const char* ProductUserId() { return ""; }
bool CopyIdToken(std::string& out) { out.clear(); return false; }
const char* Violation() { return ""; }
void ClearViolation() {}

} // namespace acclient

#endif
