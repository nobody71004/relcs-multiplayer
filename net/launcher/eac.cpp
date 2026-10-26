// net/launcher/eac.cpp — Easy Anti-Cheat install + protected launch.
#include "eac.h"

#include <windows.h>

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

	out.runtimeInstalled = runtimeOk && protOk &&
	                       FileExists(acDir + "\\EasyAntiCheat_EOS_Setup.exe") &&
	                       FileExists(gameDir + "\\start_protected_game.exe");

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
	         "runtime=%s config=%s integrity=%s%s%s",
	         out.runtimeInstalled ? "ok" : "missing",
	         out.configWritten ? "ok" : "failed",
	         out.integrityBuilt ? "signed" : "not-built",
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
	if(!FileExists(tool) || !FileExists(key) || !FileExists(cert) || !FileExists(cfg)){
		out.lastError = "integrity tooling missing under " + payload;
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
	return true;
}

bool Launch(const std::string& gameDir, const std::string& gameExeName,
            const std::string& args, std::string& error, bool& protectedLaunch)
{
	std::string protectedExe = gameDir + "\\start_protected_game.exe";
	std::string plainExe = gameDir + "\\" + gameExeName;
	bool useProtected = FileExists(protectedExe);
	std::string exe = useProtected ? protectedExe : plainExe;
	if(!FileExists(exe)){
		error = "game executable not found: " + exe;
		return false;
	}

	std::string cmd = Quote(exe);
	if(!args.empty()) cmd += " " + args;

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	BOOL ok = CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0,
	                         nullptr, gameDir.c_str(), &si, &pi);
	if(!ok){
		char buf[128];
		snprintf(buf, sizeof buf, "CreateProcess failed (error %lu)", GetLastError());
		error = buf;
		return false;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	protectedLaunch = useProtected;
	return true;
}

} // namespace eac
