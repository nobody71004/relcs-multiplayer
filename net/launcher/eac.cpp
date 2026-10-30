// net/launcher/eac.cpp — Easy Anti-Cheat install + protected launch.
#include "eac.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rnet_eos.h"

namespace eac {
namespace {

std::string g_launcherDir;   // directory of the running launcher
std::string g_payloadDir;    // directory holding the shipped eac/ payload

std::string ExeDir()
{
	if(!g_launcherDir.empty()) return g_launcherDir;
	char buf[512] = {};
	GetModuleFileNameA(nullptr, buf, sizeof buf);
	char* slash = strrchr(buf, '\\');
	if(slash) *slash = 0;
	g_launcherDir = buf;
	return g_launcherDir;
}

bool FileExists(const std::string& p)
{
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::string& p)
{
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool WriteTime(const std::string& p, FILETIME& out)
{
	HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       nullptr, OPEN_EXISTING, 0, nullptr);
	if(h == INVALID_HANDLE_VALUE) return false;
	bool ok = GetFileTime(h, nullptr, nullptr, &out) != 0;
	CloseHandle(h);
	return ok;
}

// The integrity catalogue hashes every file under the game folder except the
// paths/extensions listed in net/eac/integrity.cfg. It is only valid while it
// is newer than the newest of those files: replacing cefui.dll (or any game
// file) after signing makes EAC treat the module as tampered, and the game then
// cannot load it — the CEF HUD silently falls back to native and EOSSDK fails.
// Windows paths and the integrity tool are case-insensitive; the game writes
// e.g. userfiles/SCRDBG.LOG, which must match the "log" exclusion in
// integrity.cfg or every launch would look stale and re-sign the catalogue.
static std::string Lower(std::string s)
{
	for(char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

static bool ExcludedDir(const std::string& name)
{
	std::string n = Lower(name);
	return n == "logs" || n == "screenshots" || n == "eos-cache" ||
	       n == "licenses" || n == "localization" || n == "certificates";
}

static bool ExcludedExt(const std::string& name)
{
	static const char* kExt[] = {
		".bak", ".bat", ".bmp", ".cfg", ".db", ".eac", ".ico", ".inf", ".ini",
		".jpg", ".last", ".log", ".manifest", ".mdb", ".pdb", ".png", ".pubkey",
		".tmp", ".txt", ".vdf", ".xml", ".zip"
	};
	size_t dot = name.find_last_of('.');
	if(dot == std::string::npos) return false;   // no extension: catalogued
	std::string ext = Lower(name.substr(dot));
	for(const char* e : kExt) if(ext == e) return true;
	return false;
}

static bool NewestCataloguedFile(const std::string& dir, FILETIME& newest, int depth,
                                 std::string* newestPath = nullptr, int* scanned = nullptr)
{
	if(depth > 8) return false;
	bool any = false;
	WIN32_FIND_DATAA fd;
	std::string pattern = dir + "\\*";
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if(h == INVALID_HANDLE_VALUE) return false;
	do{
		std::string name = fd.cFileName;
		if(name == "." || name == "..") continue;
		std::string full = dir + "\\" + name;
		if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
			if(ExcludedDir(name)) continue;
			if(NewestCataloguedFile(full, newest, depth + 1, newestPath, scanned)) any = true;
			continue;
		}
		if(ExcludedExt(name)) continue;
		if(scanned) (*scanned)++;
		// Compare against the running maximum, never against this directory's
		// own "first file": recursion means newest is shared with the parent,
		// and resetting it per directory let an old backup folder hide a file
		// the launcher had just refreshed.
		if(CompareFileTime(&fd.ftLastWriteTime, &newest) > 0){
			newest = fd.ftLastWriteTime;
			if(newestPath) *newestPath = full;
		}
		any = true;
	}while(FindNextFileA(h, &fd));
	FindClose(h);
	return any;
}

bool CopyTree(const std::string& from, const std::string& to)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
	if(h == INVALID_HANDLE_VALUE) return false;
	bool ok = true;
	do{
		if(!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		std::string src = from + "\\" + fd.cFileName;
		std::string dst = to + "\\" + fd.cFileName;
		if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
			if(!DirExists(dst)) CreateDirectoryA(dst.c_str(), nullptr);
			ok = CopyTree(src, dst) && ok;
		}else{
			SetFileAttributesA(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
			if(!CopyFileA(src.c_str(), dst.c_str(), FALSE)){
				// an EasyAntiCheat service may hold its own files: keep going
				ok = false;
			}
		}
	}while(FindNextFileA(h, &fd));
	FindClose(h);
	return ok;
}

bool WriteText(const std::string& path, const std::string& text)
{
	FILE* f = fopen(path.c_str(), "wb");
	if(!f) return false;
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	return true;
}

std::string ReadFile(const std::string& path)
{
	std::string out;
	FILE* f = fopen(path.c_str(), "rb");
	if(!f) return out;
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
	fclose(f);
	return out;
}

// The eac payload ships next to the launcher (shipped installs) or one/two
// levels up (the net/bin/<cfg> build tree). Probe both.
std::string FindPayload()
{
	if(!g_payloadDir.empty()) return g_payloadDir;
	std::string dir = ExeDir();
	const char* rel[] = { "\\eac", "\\..\\eac", "\\..\\..\\eac" };
	for(const char* r : rel){
		std::string p = dir + r;
		if(FileExists(p + "\\runtime\\start_protected_game.exe")){
			g_payloadDir = p;
			return p;
		}
	}
	g_payloadDir = dir + "\\eac";
	return g_payloadDir;
}

bool CopyIfNewer(const std::string& src, const std::string& dst, bool& copied)
{
	copied = false;
	HANDLE hs = CreateFileA(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                        OPEN_EXISTING, 0, nullptr);
	if(hs == INVALID_HANDLE_VALUE) return false;
	FILETIME srcWrite{};
	GetFileTime(hs, nullptr, nullptr, &srcWrite);
	CloseHandle(hs);

	HANDLE hd = CreateFileA(dst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                        OPEN_EXISTING, 0, nullptr);
	bool need = true;
	if(hd != INVALID_HANDLE_VALUE){
		FILETIME dstWrite{};
		GetFileTime(hd, nullptr, nullptr, &dstWrite);
		CloseHandle(hd);
		need = CompareFileTime(&srcWrite, &dstWrite) > 0;
	}
	if(!need) return true;
	SetFileAttributesA(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
	if(!CopyFileA(src.c_str(), dst.c_str(), FALSE)) return false;
	copied = true;
	return true;
}

bool RunTool(const std::string& exe, const std::string& cmdline, const std::string& cwd,
             const std::string& logPath, DWORD& exitCode)
{
	std::string full = "\"" + exe + "\"" + cmdline;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	si.hStdOutput = log;
	si.hStdError = log;
	si.hStdInput = nullptr;
	BOOL ok = log != INVALID_HANDLE_VALUE &&
	          CreateProcessA(nullptr, (LPSTR)full.c_str(), nullptr, nullptr, TRUE,
	                         0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
	if(log != INVALID_HANDLE_VALUE) CloseHandle(log);
	if(!ok){
		exitCode = GetLastError();
		return false;
	}
	WaitForSingleObject(pi.hProcess, 120000);
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
}

std::string Quote(const std::string& s)
{
	return "\"" + s + "\"";
}

} // namespace

bool Prepare(const std::string& gameDir, const std::string& gameExeName, Status& out)
{
	out = Status();
	std::string payload = FindPayload();
	std::string logPath = payload + "\\eac-prepare.log";

	if(!DirExists(gameDir)){
		out.lastError = "game directory not found: " + gameDir;
		return false;
	}

	bool anyCopied = false;
	std::string runtimeSrc = payload + "\\runtime";
	if(!DirExists(runtimeSrc)){
		out.lastError = "anti-cheat payload missing (" + runtimeSrc + ")";
		return false;
	}

	// EasyAntiCheat/ runtime (service binaries, splash screen, localisation)
	std::string acDir = gameDir + "\\EasyAntiCheat";
	if(!DirExists(acDir)) CreateDirectoryA(acDir.c_str(), nullptr);
	bool runtimeOk = true;
	// the UI polls the status; do not recopy the localisation tree on every poll
	bool haveRuntime = FileExists(acDir + "\\EasyAntiCheat_EOS_Setup.exe") &&
	                   FileExists(acDir + "\\SplashScreen.png");
	if(!haveRuntime){
		for(const char* file : { "EasyAntiCheat_EOS_Setup.exe", "SplashScreen.png" })
			if(FileExists(runtimeSrc + "\\EasyAntiCheat\\" + file)){
				bool copied = false;
				runtimeOk = CopyIfNewer(runtimeSrc + "\\EasyAntiCheat\\" + file,
				                        acDir + "\\" + file, copied) && runtimeOk;
				anyCopied = anyCopied || copied;
			}
		for(const char* sub : { "Licenses", "Localization" }){
			std::string src = runtimeSrc + "\\EasyAntiCheat\\" + sub;
			if(DirExists(src)){
				if(!DirExists(acDir + "\\" + sub)) CreateDirectoryA((acDir + "\\" + sub).c_str(), nullptr);
				CopyTree(src, acDir + "\\" + sub);
			}
		}
	}

	// the protected bootstrapper sits next to the game executable
	bool protOk = true;
	if(FileExists(runtimeSrc + "\\start_protected_game.exe")){
		bool copied = false;
		protOk = CopyIfNewer(runtimeSrc + "\\start_protected_game.exe",
		                     gameDir + "\\start_protected_game.exe", copied);
		anyCopied = anyCopied || copied;
	}

	// A signed catalogue is what lets the bootstrapper start the game at all.
	// The package carries one signed on the build host; install it into a game
	// folder that has none, but never replace a catalogue signed locally for a
	// different build.
	std::string certSrc = runtimeSrc + "\\EasyAntiCheat\\Certificates";
	std::string certDst = acDir + "\\Certificates";
	if(DirExists(certSrc) && !FileExists(certDst + "\\base.bin")){
		if(!DirExists(certDst)) CreateDirectoryA(certDst.c_str(), nullptr);
		CopyTree(certSrc, certDst);
		anyCopied = true;
	}

	out.runtimeInstalled = runtimeOk && protOk &&
	                       FileExists(acDir + "\\EasyAntiCheat_EOS_Setup.exe") &&
	                       FileExists(gameDir + "\\start_protected_game.exe");

	// The in-game anti-cheat client (src/net/acclient.cpp) loads the EOS SDK
	// from the game folder: without it the client cannot open an AC session and
	// a server that requires protection would refuse it. Same file the launcher
	// uses, so a shipped launcher install carries it.
	const char* sdkName = "EOSSDK-Win64-Shipping.dll";
	std::string sdkDst = gameDir + "\\" + sdkName;
	bool sdkOk = FileExists(sdkDst);
	if(!sdkOk){
		const std::string candidates[] = {
			ExeDir() + "\\" + sdkName,
			ExeDir() + "\\..\\..\\..\\vendor\\eos-sdk\\bin\\Win64\\" + sdkName,
			payload + "\\..\\..\\vendor\\eos-sdk\\bin\\Win64\\" + sdkName,
		};
		for(const std::string& src : candidates){
			if(!FileExists(src)) continue;
			bool copied = false;
			sdkOk = CopyIfNewer(src, sdkDst, copied);
			anyCopied = anyCopied || copied;
			if(sdkOk) break;
		}
	}
	out.eosSdk = sdkOk;

	// client credentials for the Connect device login. The launcher already has
	// them on this machine; the game needs its own copy next to the SDK.
	std::string iniSrc = ExeDir() + "\\eos.ini";
	if(FileExists(iniSrc)){
		std::string iniDst = gameDir + "\\eos.ini";
		if(ReadFile(iniDst) != ReadFile(iniSrc)){
			bool copied = false;
			CopyIfNewer(iniSrc, iniDst, copied);
			anyCopied = anyCopied || copied;
		}
	}
	out.eosConfig = FileExists(gameDir + "\\eos.ini");

	// EOS product config consumed by the bootstrapper
	char settings[1024];
	snprintf(settings, sizeof settings,
	         "{\r\n"
	         "\t\"title\"\t\t\t: \"%s\",\r\n"
	         "\t\"executable\"\t\t: \"%s\",\r\n"
	         "\t\"productid\"\t\t: \"%s\",\r\n"
	         "\t\"sandboxid\"\t\t: \"%s\",\r\n"
	         "\t\"deploymentid\"\t\t: \"%s\",\r\n"
	         "\t\"requested_splash\"\t: \"EasyAntiCheat/SplashScreen.png\",\r\n"
	         "\t\"wait_for_game_process_exit\"\t: \"false\",\r\n"
	         "\t\"hide_bootstrapper\"\t: \"false\",\r\n"
	         "\t\"hide_gui\"\t\t: \"false\"\r\n"
	         "}\r\n",
	         rnet::EOS_GAME_TITLE, gameExeName.c_str(), rnet::EOS_PRODUCT_ID,
	         rnet::EOS_SANDBOX_ID, rnet::EOS_DEPLOYMENT_ID);
	std::string settingsPath = acDir + "\\Settings.json";
	bool settingsOk = true;
	std::string currentSettings = ReadFile(settingsPath);
	if(currentSettings != settings) settingsOk = WriteText(settingsPath, settings);

	// the CDN config that tells EAC where to fetch its module build
	std::string eacCfg = ReadFile(payload + "\\reLCS.exe.eac");
	if(eacCfg.empty())
		eacCfg = "cdn=https://modules-cdn.eac-prod.on.epicgames.com/builds/14006/"
		         "a4e0e875-fbff-4e7e-a823-8f0a18b1321f/{{system}}";
	std::string cfgName = gameDir + "\\" + gameExeName + ".eac";
	bool cfgOk = true;
	if(ReadFile(cfgName) != eacCfg) cfgOk = WriteText(cfgName, eacCfg);

	out.configWritten = settingsOk && cfgOk;
	out.integrityBuilt = FileExists(acDir + "\\Certificates\\base.bin");

	// A catalogue signed before a game file changed is the most common
	// protected-launch failure: EAC hashes the game files, the hash no longer
	// matches what base.bin recorded, and it refuses the launch (210: Unrecognized
	// game client) or silently refuses to load the changed module. Comparing only
	// the executable missed exactly that case: the launcher refreshed cefui.dll
	// after the last signing, so the protected game could not load its HUD.
	// Compare against every file the catalogue covers, not just the exe.
	if(out.integrityBuilt){
		FILETIME binT{}, newest{};
		std::string newestPath;
		if(WriteTime(acDir + "\\Certificates\\base.bin", binT) &&
		   NewestCataloguedFile(gameDir, newest, 0, &newestPath) &&
		   CompareFileTime(&newest, &binT) > 0){
			out.staleIntegrity = true;
			out.lastError = "integrity catalogue is stale: " + newestPath +
			                " is newer than EasyAntiCheat/Certificates/base.bin, so Easy "
			                "Anti-Cheat refuses to load it (cefui.dll then falls back to the "
			                "native HUD) — re-sign it with: reLCS-launcher.exe -game " +
			                gameDir + "\\" + gameExeName + " -eacintegrity";
		}
	}

	{
		std::string ver = ReadFile(acDir + "\\EasyAntiCheat_EOS_Setup.exe");
		(void)ver;
	}
	char vbuf[64] = {};
	FILE* vf = fopen((payload + "\\.version").c_str(), "rb");
	if(vf){
		size_t n = fread(vbuf, 1, sizeof vbuf - 1, vf);
		vbuf[n] = 0;
		fclose(vf);
		while(n && (vbuf[n - 1] == '\n' || vbuf[n - 1] == '\r')) vbuf[--n] = 0;
		out.version = vbuf;
	}

	char summary[512];
	snprintf(summary, sizeof summary,
	         "runtime=%s config=%s integrity=%s eos-sdk=%s eos-ini=%s%s%s",
	         out.runtimeInstalled ? "ok" : "missing",
	         out.configWritten ? "ok" : "failed",
	         out.integrityBuilt ? (out.staleIntegrity ? "stale" : "signed") : "not-built",
	         out.eosSdk ? "ok" : "missing",
	         out.eosConfig ? "ok" : "missing",
	         anyCopied ? " (files refreshed)" : "",
	         runtimeOk && protOk ? "" : " (some files locked — close the game)");
	out.log = summary;
	return out.runtimeInstalled && out.configWritten;
}

bool InstallService(const std::string& gameDir, Status& out)
{
	std::string setup = gameDir + "\\EasyAntiCheat\\EasyAntiCheat_EOS_Setup.exe";
	if(!FileExists(setup)){
		out.lastError = "EasyAntiCheat_EOS_Setup.exe missing";
		return false;
	}
	DWORD rc = 0;
	std::string logPath = gameDir + "\\EasyAntiCheat\\install.log";
	bool ran = RunTool(setup, " install " + std::string(rnet::EOS_PRODUCT_ID), gameDir, logPath, rc);
	char summary[256];
	snprintf(summary, sizeof summary, "EAC service install exit=%lu%s", rc, ran ? "" : " (spawn failed)");
	out.log = summary;
	if(!ran || rc != 0){
		out.lastError = ran ? "EAC service install failed (run the launcher as administrator)"
		                    : "could not start EasyAntiCheat_EOS_Setup.exe";
		return false;
	}
	return true;
}

bool BuildIntegrity(const std::string& gameDir, const std::string& gameExeName, Status& out)
{
	std::string payload = FindPayload();
	std::string tool = payload + "\\devtools\\anticheat_integritytool.exe";
	std::string cfg = payload + "\\integrity.cfg";
	std::string key = payload + "\\keys\\base_private.key";
	std::string cert = payload + "\\keys\\base_public.cer";
	if(!FileExists(tool) || !FileExists(cfg)){
		out.lastError = "integrity tool missing under " + payload +
		                " (the release package must ship eac/devtools)";
		return false;
	}
	if(!FileExists(key) || !FileExists(cert)){
		// By design: package-release.ps1 strips the private key so a shipped
		// launcher can never re-sign. Say that, instead of reporting the whole
		// toolchain as missing.
		out.lastError = "signing key not shipped (eac/keys/base_private.key is build-machine "
		                "only) — sign the catalogue on the build host with -eacintegrity, "
		                "then ship EasyAntiCheat/Certificates";
		return false;
	}
	if(!FileExists(gameDir + "\\" + gameExeName)){
		out.lastError = "game executable missing: " + gameDir + "\\" + gameExeName;
		return false;
	}

	std::string cmdline =
	    " -productid " + std::string(rnet::EOS_PRODUCT_ID) +
	    " -inkey " + Quote(key) +
	    " -incert " + Quote(cert) +
	    " -outname base" +
	    " -target_game_dir " + Quote(gameDir) +
	    " " + Quote(cfg);
	DWORD rc = 0;
	std::string logPath = payload + "\\integrity.log";
	bool ran = RunTool(tool, cmdline, payload + "\\devtools", logPath, rc);
	out.integrityBuilt = FileExists(gameDir + "\\EasyAntiCheat\\Certificates\\base.bin");
	char summary[256];
	snprintf(summary, sizeof summary, "integrity tool exit=%lu signed=%s",
	         rc, out.integrityBuilt ? "yes" : "no");
	out.log = summary;
	if(!ran || rc != 0 || !out.integrityBuilt){
		out.lastError = "integrity catalogue was not signed (see integrity.log)";
		return false;
	}
	// freshly signed: Prepare() may have flagged the previous catalogue as
	// stale, and that warning no longer applies.
	out.lastError.clear();
	out.staleIntegrity = false;
	return true;
}

// True when the bootstrapper has actually spawned the game. EAC keeps its
// splash/error UI alive for a while after a refusal, so the bootstrapper's exit
// code alone cannot be trusted: look for a game process created after the
// launch. The parent check covers the normal case; the creation time covers
// EAC starting the game through its service.
static bool GameStarted(const std::string& exeName, DWORD parentPid, const FILETIME& launchedAt)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(snap == INVALID_HANDLE_VALUE) return false;
	// the current SDK only exposes the wide toolhelp API
	wchar_t want[MAX_PATH] = {};
	MultiByteToWideChar(CP_ACP, 0, exeName.c_str(), -1, want, MAX_PATH);
	PROCESSENTRY32W pe;
	pe.dwSize = sizeof pe;
	bool found = false;
	if(Process32FirstW(snap, &pe)){
		do{
			if(_wcsicmp(pe.szExeFile, want) != 0) continue;
			if(pe.th32ParentProcessID == parentPid){ found = true; break; }
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
			if(h){
				FILETIME created{}, exited{}, kernel{}, user{};
				if(GetProcessTimes(h, &created, &exited, &kernel, &user) &&
				   CompareFileTime(&created, &launchedAt) >= 0)
					found = true;
				CloseHandle(h);
			}
			if(found) break;
		}while(Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return found;
}

bool Launch(const std::string& gameDir, const std::string& gameExeName,
            const std::string& args, std::string& error, bool& protectedLaunch,
            bool allowProtected)
{
	std::string protectedExe = gameDir + "\\start_protected_game.exe";
	std::string plainExe = gameDir + "\\" + gameExeName;
	bool useProtected = allowProtected && FileExists(protectedExe);
	if(!FileExists(useProtected ? protectedExe : plainExe)){
		error = "game executable not found: " + (useProtected ? protectedExe : plainExe);
		return false;
	}

	// Always try the protected bootstrapper first. A stale catalogue or a
	// missing EasyAntiCheat service makes it refuse (and show its own error
	// dialog) instead of starting the game; only then start the game directly so
	// the player still gets in — with the EAC state reported by the caller.
	for(int attempt = 0; attempt < 2; attempt++){
		bool asProtected = useProtected && attempt == 0;
		std::string exe = asProtected ? protectedExe : plainExe;
		std::string cmd = Quote(exe);
		if(!args.empty()) cmd += " " + args;

		STARTUPINFOA si;
		PROCESS_INFORMATION pi;
		memset(&si, 0, sizeof si);
		memset(&pi, 0, sizeof pi);
		si.cb = sizeof si;
		FILETIME launchedAt;
		GetSystemTimeAsFileTime(&launchedAt);
		BOOL ok = CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0,
		                         nullptr, gameDir.c_str(), &si, &pi);
		if(!ok){
			char buf[128];
			snprintf(buf, sizeof buf, "CreateProcess failed (error %lu)", GetLastError());
			error = buf;
			return false;
		}

		if(!asProtected){
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			protectedLaunch = false;
			return true;
		}

		// EAC's splash + service start can take a few seconds
		bool started = false;
		DWORD exitCode = STILL_ACTIVE;
		ULONGLONG t0 = GetTickCount64();
		while(GetTickCount64() - t0 < 20000){
			if(GameStarted(gameExeName, pi.dwProcessId, launchedAt)){ started = true; break; }
			if(GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) break;
			Sleep(150);
		}

		if(started){
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			protectedLaunch = true;
			return true;
		}

		// refused: stop the bootstrapper so its error dialog does not linger
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		error = "the Easy Anti-Cheat bootstrapper did not start the game";
		protectedLaunch = false;
	}

	return false;
}

} // namespace eac
