// net/launcher/eos_bootstrap.cpp — EOS platform init + Connect Device ID login.
//
// Everything here runs on one worker thread: EOS_Platform_Tick has to be pumped
// for the async Connect calls to complete, and doing it off the launcher UI
// thread keeps the server browser responsive while the SDK boots.
#include "eos_bootstrap.h"

#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_common.h>
#include <eos_logging.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace eosboot {
namespace {

struct Ctx {
	std::mutex lock;
	Status st;

	std::atomic<bool> running{false};
	std::atomic<bool> stop{false};
	std::thread worker;

	EOS_HPlatform platform = nullptr;
	EOS_HConnect connect = nullptr;

	std::atomic<int> deviceResult{-2};   // -2 pending, else EOS_EResult
	std::atomic<int> loginResult{-2};
	EOS_ProductUserId puid = nullptr;
};

Ctx g;
FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
	if(!g_log){
		g_log = fopen("eos-bootstrap.log", "w");
		if(!g_log) return;
	}
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(g_log, "%s\n", buf);
	fflush(g_log);
}

void SetPending(const char* step)
{
	std::lock_guard<std::mutex> lk(g.lock);
	g.st.pendingStep = step ? step : "";
}

void AddStep(const char* step)
{
	std::lock_guard<std::mutex> lk(g.lock);
	std::string s = step ? step : "";
	if(s.empty()) return;
	if(!g.st.steps.empty()) g.st.steps += "|";
	g.st.steps += s;
	g.st.pendingStep.clear();
}

void SetError(const char* error)
{
	std::lock_guard<std::mutex> lk(g.lock);
	g.st.error = error ? error : "";
}

const char* ResultName(int result)
{
	switch(result){
	case (int)EOS_EResult::EOS_Success: return "success";
	case (int)EOS_EResult::EOS_DuplicateNotAllowed: return "already-exists";
	case (int)EOS_EResult::EOS_NotFound: return "not-found";
	case (int)EOS_EResult::EOS_InvalidUser: return "invalid-user";
	case (int)EOS_EResult::EOS_InvalidParameters: return "invalid-parameters";
	case (int)EOS_EResult::EOS_NotConfigured: return "not-configured";
	case (int)EOS_EResult::EOS_NoConnection: return "no-connection";
	case (int)EOS_EResult::EOS_TimedOut: return "timed-out";
	case (int)EOS_EResult::EOS_ServiceFailure: return "service-failure";
	default: return EOS_EResult_ToString((EOS_EResult)result);
	}
}

void EOS_CALL OnCreateDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo* Data)
{
	Log("[eos] CreateDeviceId result=%s", ResultName((int)Data->ResultCode));
	g.deviceResult = (int)Data->ResultCode;
}

void EOS_CALL OnLogin(const EOS_Connect_LoginCallbackInfo* Data)
{
	Log("[eos] Connect login result=%s", ResultName((int)Data->ResultCode));
	if(Data->ResultCode == EOS_EResult::EOS_Success && Data->LocalUserId){
		g.puid = Data->LocalUserId;
		g.loginResult = 0;
	}else{
		g.loginResult = (int)Data->ResultCode;
	}
}

void EOS_CALL OnEosLog(const EOS_LogMessage* msg)
{
	if(!msg || !msg->Message || msg->Level == EOS_ELogLevel::EOS_LOG_Off) return;
	if(msg->Level == EOS_ELogLevel::EOS_LOG_Error || msg->Level == EOS_ELogLevel::EOS_LOG_Fatal)
		Log("[eos-sdk] ERROR %s: %s", msg->Category ? msg->Category : "?", msg->Message);
	else if(msg->Level == EOS_ELogLevel::EOS_LOG_Warning)
		Log("[eos-sdk] warn %s: %s", msg->Category ? msg->Category : "?", msg->Message);
}

bool InitSdk()
{
	EOS_InitializeOptions opts = {};
	opts.ApiVersion = EOS_INITIALIZE_API_LATEST;
	opts.ProductName = "reLCS";
	opts.ProductVersion = "1.0.0";
	EOS_EResult rc = EOS_Initialize(&opts);
	if(rc != EOS_EResult::EOS_Success){
		Log("[eos] EOS_Initialize failed: %s", EOS_EResult_ToString(rc));
		SetError((std::string("EOS_Initialize failed: ") + EOS_EResult_ToString(rc)).c_str());
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(g.lock);
		g.st.sdkLoaded = true;
		g.st.version = "1.19.2";
	}
	Log("[eos] SDK %s initialized", "1.19.2");
	EOS_Logging_SetCallback(OnEosLog);
	EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
	AddStep("sdk");
	return true;
}

bool CreatePlatform(const std::string& productId, const std::string& sandboxId,
                    const std::string& deploymentId, const std::string& clientId,
                    const std::string& clientSecret)
{
	if(clientId.empty() || clientSecret.empty()){
		Log("[eos] client credentials missing (product -> Clients in the dev portal)");
		SetError("EOS client credentials missing — Dev Portal → Product → Clients → Create Client, then put client_id/client_secret in eos.ini");
		AddStep("client-credentials-missing");
		return false;
	}

	EOS_Platform_Options opts = {};
	opts.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
	opts.ProductId = productId.c_str();
	opts.SandboxId = sandboxId.c_str();
	opts.DeploymentId = deploymentId.c_str();
	opts.bIsServer = EOS_FALSE;
	opts.ClientCredentials.ClientId = clientId.c_str();
	opts.ClientCredentials.ClientSecret = clientSecret.c_str();
	// EOS 1.19 requires an absolute cache path; a relative one makes
	// EOS_Platform_Create fail with "Cache directory path is relative".
	static std::string cacheDir = []{
		char exe[MAX_PATH] = {};
		DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
		std::string dir;
		if(n > 0 && n < MAX_PATH){
			dir.assign(exe, n);
			size_t slash = dir.find_last_of("\\/");
			if(slash != std::string::npos) dir.resize(slash + 1);
		}
		dir += "eos-cache";
		CreateDirectoryA(dir.c_str(), nullptr);
		return dir;
	}();
	opts.CacheDirectory = cacheDir.c_str();
	opts.TickBudgetInMilliseconds = 0;
	opts.Flags = EOS_PF_DISABLE_OVERLAY;   // the launcher has no social overlay

	g.platform = EOS_Platform_Create(&opts);
	if(!g.platform){
		Log("[eos] EOS_Platform_Create failed");
		SetError("EOS_Platform_Create failed (check product/sandbox/deployment ids)");
		return false;
	}
	g.connect = EOS_Platform_GetConnectInterface(g.platform);
	if(!g.connect){
		Log("[eos] no Connect interface");
		SetError("EOS Connect interface unavailable");
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(g.lock);
		g.st.platformReady = true;
	}
	Log("[eos] platform ready (product=%s sandbox=%s deployment=%s)",
	    productId.c_str(), sandboxId.c_str(), deploymentId.c_str());
	AddStep("platform");
	return true;
}

void CreateDeviceId()
{
	EOS_Connect_CreateDeviceIdOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
	opts.DeviceModel = "PC Windows";
	SetPending("device-id");
	g.deviceResult = -2;
	EOS_Connect_CreateDeviceId(g.connect, &opts, nullptr, OnCreateDeviceId);
	Log("[eos] CreateDeviceId request sent (model=%s)", opts.DeviceModel);
}

void Login(const std::string& displayName)
{
	EOS_Connect_Credentials creds = {};
	creds.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
	creds.Token = "";
	creds.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;

	EOS_Connect_UserLoginInfo info = {};
	info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
	info.DisplayName = displayName.c_str();

	EOS_Connect_LoginOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
	opts.Credentials = &creds;
	opts.UserLoginInfo = &info;

	SetPending("connect-login");
	g.loginResult = -2;
	EOS_Connect_Login(g.connect, &opts, nullptr, OnLogin);
}

void Shutdown()
{
	if(g.platform){
		EOS_Platform_Release(g.platform);
		g.platform = nullptr;
	}
	if(g_log){
		EOS_Shutdown();
	}
	if(g_log){
		fclose(g_log);
		g_log = nullptr;
	}
}

} // namespace

void Start(const std::string& productId, const std::string& sandboxId,
           const std::string& deploymentId, const std::string& clientId,
           const std::string& clientSecret)
{
	if(g.running.exchange(true)){
		Log("[eos] bootstrap already running");
		return;
	}
	g.stop = false;
	g.deviceResult = -2;
	g.loginResult = -2;

	g.worker = std::thread([productId, sandboxId, deploymentId, clientId, clientSecret]{
		if(!InitSdk()){ g.running = false; return; }
		AddStep("sdk-init");
		if(!CreatePlatform(productId, sandboxId, deploymentId, clientId, clientSecret)){
			Shutdown();
			g.running = false;
			return;
		}

		bool deviceIssued = false;
		bool loginSent = false;
		double loginAt = 0.0;
		auto started = std::chrono::steady_clock::now();

		while(!g.stop){
			EOS_Platform_Tick(g.platform);

			double elapsed = std::chrono::duration<double>(
			    std::chrono::steady_clock::now() - started).count();

			if(!deviceIssued){
				CreateDeviceId();
				deviceIssued = true;
			}
			if(!loginSent){
				int dr = g.deviceResult.load();
				// DuplicateNotAllowed means this machine already has a device id:
				// that is the normal path on every start after the first.
				if(dr >= 0 && dr != (int)EOS_EResult::EOS_Success &&
				   dr != (int)EOS_EResult::EOS_DuplicateNotAllowed){
					Log("[eos] CreateDeviceId failed: %s", ResultName(dr));
					SetError((std::string("CreateDeviceId failed: ") + ResultName(dr) +
					          " (enable Connect for the client policy in the dev portal)").c_str());
					AddStep("device-id-failed");
					break;
				}
				if(dr == (int)EOS_EResult::EOS_Success ||
				   dr == (int)EOS_EResult::EOS_DuplicateNotAllowed){
					AddStep("device-id");
					Login("reLCS Player");
					loginSent = true;
					loginAt = elapsed;
				}else if(elapsed > 60.0){
					Log("[eos] CreateDeviceId timed out");
					SetError("CreateDeviceId timed out (Connect permission, network or sandbox/deployment mismatch)");
					AddStep("device-id-timeout");
					break;
				}
			}else{
				int lr = g.loginResult.load();
				if(lr == 0){
					char buf[64] = {};
					int32_t len = (int32_t)sizeof buf;
					EOS_ProductUserId_ToString(g.puid, buf, &len);
					{
						std::lock_guard<std::mutex> lk(g.lock);
						g.st.loggedIn = true;
						g.st.productUserId = len > 0 ? buf : "";
						g.st.displayName = "reLCS Player";
					}
					AddStep("connect-login");
					Log("[eos] logged in as %s", buf);
					break;
				}
				if(lr != -2){
					Log("[eos] login failed: %s", ResultName(lr));
					SetError((std::string("Connect login failed: ") + ResultName(lr)).c_str());
					AddStep("connect-login-failed");
					break;
				}
				if(elapsed - loginAt > 20.0){
					Log("[eos] login timed out");
					SetError("Connect login timed out");
					AddStep("connect-login-timeout");
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}

		// keep ticking so presence/overlay stay alive while the launcher runs
		while(!g.stop)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		Shutdown();
		g.running = false;
	});
	g.worker.detach();
}

void Stop()
{
	g.stop = true;
}

Status Get()
{
	std::lock_guard<std::mutex> lk(g.lock);
	return g.st;
}

std::string Json()
{
	Status s = Get();
	std::string steps;
	size_t start = 0;
	while(start <= s.steps.size()){
		size_t end = s.steps.find('|', start);
		std::string one = s.steps.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if(!one.empty()){
			if(!steps.empty()) steps += ",";
			steps += "\"" + one + "\"";
		}
		if(end == std::string::npos) break;
		start = end + 1;
	}
	std::string js = "{";
	js += "\"ok\":" + std::string(s.loggedIn ? "true" : "false");
	js += ",\"sdk\":" + std::string(s.sdkLoaded ? "true" : "false");
	js += ",\"platform\":" + std::string(s.platformReady ? "true" : "false");
	js += ",\"version\":\"" + s.version + "\"";
	js += ",\"puid\":\"" + s.productUserId + "\"";
	js += ",\"name\":\"" + s.displayName + "\"";
	js += ",\"error\":\"" + s.error + "\"";
	js += ",\"pending\":\"" + s.pendingStep + "\"";
	js += ",\"steps\":[" + steps + "]";
	js += "}";
	return js;
}

} // namespace eosboot
