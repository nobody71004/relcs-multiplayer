// net/server/anticheat_config.h — EOS Anti-Cheat server settings + the tiny
// config-file readers that fill them in.
//
// Header-only and SDK-free on purpose: the server build pulls in
// anticheat.cpp for the real SDK work, while net-tests can cover the parsing
// and validation below without the EOS SDK present.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ac {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Config {
	bool enabled = true;                 // eac=0 turns the whole integration off
	std::string serverName = "reLCS";    // shown in the EOS AC dashboard
	std::string productId;               // default: EAC Settings.json / built-in
	std::string sandboxId;
	std::string deploymentId;
	std::string clientId;                // default: eos.ini next to the server
	std::string clientSecret;
	std::string cacheDir = "eos-cache";  // made absolute before EOS sees it
	std::string sdkDir;                  // extra directory to search for the SDK
	std::string settingsPath;            // explicit EAC Settings.json ("" = search)
	std::string iniPath;                 // explicit eos.ini ("" = search)
	uint32_t registerTimeout = 60;       // seconds EOS gives a client to authenticate
	bool enableGameplayData = false;     // LogPlayerTick & friends: not used here
	bool kickOnViolation = true;         // EOS "remove player" -> reLCS kick
	bool requireToken = true;            // require a verified Connect ID token

	// "only protected clients are allowed": a peer that does not register with
	// the AC session inside graceSeconds is removed. failOpen keeps the server
	// playable when the session cannot start at all (no SDK / no credentials /
	// EOS refusing every registration because the product is misconfigured)
	// instead of locking every player out of it.
	bool requireProtected = true;
	bool failOpen = true;
	uint32_t graceSeconds = 30;
};

// shipexed product data: the same ids the launcher writes into
// EasyAntiCheat/Settings.json (see net/eac/README.md)
constexpr const char* DEFAULT_PRODUCT_ID    = "fb155afe4a754a4b91929491873c844e";
constexpr const char* DEFAULT_SANDBOX_ID    = "515e78d9d395488faf0a3f63041dbba0";
constexpr const char* DEFAULT_DEPLOYMENT_ID = "a287ae738e5241829bdcd1d8a0d3b0bb";

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
inline bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string Trim(const std::string& in)
{
	size_t a = 0, b = in.size();
	while(a < b && IsSpace(in[a])) a++;
	while(b > a && IsSpace(in[b - 1])) b--;
	return in.substr(a, b - a);
}

// "client_id = abc  # comment" -> "abc"
inline bool ReadIniValue(const std::string& path, const std::string& key, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if(!f) return false;
	char line[512];
	bool found = false;
	while(std::fgets(line, sizeof line, f)){
		std::string s(line);
		s = Trim(s);
		if(s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;
		size_t eq = s.find('=');
		if(eq == std::string::npos) continue;
		if(Trim(s.substr(0, eq)) != key) continue;
		std::string v = Trim(s.substr(eq + 1));
		// strip a trailing comment when it is separated by whitespace
		for(size_t i = 0; i < v.size(); i++){
			if((v[i] == '#' || v[i] == ';') && i > 0 && IsSpace(v[i - 1])){
				v = Trim(v.substr(0, i));
				break;
			}
		}
		out = v;
		found = true;
		break;
	}
	std::fclose(f);
	return found;
}

// Reads the first string value for "key" out of a small JSON file
// (EasyAntiCheat/Settings.json). Deliberately not a JSON parser: the file is
// flat key/value data written by the launcher and by the dev portal.
inline bool ReadJsonString(const std::string& path, const std::string& key, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if(!f) return false;
	std::string text;
	char buf[4096];
	size_t n;
	while((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
	std::fclose(f);

	std::string needle = "\"" + key + "\"";
	size_t pos = 0;
	while((pos = text.find(needle, pos)) != std::string::npos){
		size_t p = pos + needle.size();
		while(p < text.size() && IsSpace(text[p])) p++;
		if(p >= text.size() || text[p] != ':'){ pos = p; continue; }
		p++;
		while(p < text.size() && IsSpace(text[p])) p++;
		if(p >= text.size() || text[p] != '"'){ pos = p; continue; }
		p++;
		std::string v;
		while(p < text.size() && text[p] != '"'){
			if(text[p] == '\\' && p + 1 < text.size()){ v += text[p + 1]; p += 2; continue; }
			v += text[p++];
		}
		out = v;
		return true;
	}
	return false;
}

// EOS ProductUserId strings are 32 lowercase hex characters.
inline bool IsProductUserId(const std::string& s)
{
	if(s.size() != 32) return false;
	for(char c : s){
		bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if(!hex) return false;
	}
	return true;
}

inline const char* PlatformName(uint8_t p)
{
	switch(p){
	case 1: return "windows";
	case 2: return "mac";
	case 3: return "linux";
	default: return "unknown";
	}
}

// ---------------------------------------------------------------------------
// Loading / merging
// ---------------------------------------------------------------------------
inline bool FileExists(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if(!f) return false;
	std::fclose(f);
	return true;
}

// First existing path out of the candidates, or "".
inline std::string FindFirst(const std::vector<std::string>& candidates)
{
	for(const auto& c : candidates)
		if(FileExists(c)) return c;
	return std::string();
}

// Fills product/sandbox/deployment ids from the EAC Settings.json if present.
inline void ApplySettingsJson(Config& cfg, const std::string& path)
{
	if(path.empty()) return;
	std::string v;
	if(cfg.productId.empty() && ReadJsonString(path, "productid", v)) cfg.productId = v;
	if(cfg.sandboxId.empty() && ReadJsonString(path, "sandboxid", v)) cfg.sandboxId = v;
	if(cfg.deploymentId.empty() && ReadJsonString(path, "deploymentid", v)) cfg.deploymentId = v;
	if(cfg.serverName == "reLCS" && ReadJsonString(path, "title", v) && !v.empty())
		cfg.serverName = v;
}

// Fills the client credential pair from eos.ini if present.
inline void ApplyIni(Config& cfg, const std::string& path)
{
	if(path.empty()) return;
	std::string v;
	if(cfg.clientId.empty() && ReadIniValue(path, "client_id", v)) cfg.clientId = v;
	if(cfg.clientSecret.empty() && ReadIniValue(path, "client_secret", v)) cfg.clientSecret = v;
}

// Everything the server needs to reach a session, resolved in one place.
// Clamps the values a server.cfg can get wrong, so a typo cannot turn the
// grace window into "kick on join" or "never kick".
inline void Sanitize(Config& cfg)
{
	if(cfg.graceSeconds < 5) cfg.graceSeconds = 5;
	if(cfg.graceSeconds > 600) cfg.graceSeconds = 600;
	if(cfg.registerTimeout < 10) cfg.registerTimeout = 10;
}

inline void Resolve(Config& cfg)
{
	Sanitize(cfg);
	if(cfg.productId.empty()) cfg.productId = DEFAULT_PRODUCT_ID;
	if(cfg.sandboxId.empty()) cfg.sandboxId = DEFAULT_SANDBOX_ID;
	if(cfg.deploymentId.empty()) cfg.deploymentId = DEFAULT_DEPLOYMENT_ID;

	if(cfg.settingsPath.empty())
		cfg.settingsPath = FindFirst({
			"EasyAntiCheat/Settings.json",
			"eac/runtime/EasyAntiCheat/Settings.json",
			"../eac/runtime/EasyAntiCheat/Settings.json",
			"../net/eac/runtime/EasyAntiCheat/Settings.json",
			"net/eac/runtime/EasyAntiCheat/Settings.json",
		});
	ApplySettingsJson(cfg, cfg.settingsPath);

	if(cfg.iniPath.empty())
		cfg.iniPath = FindFirst({
			"eos.ini",
			"../eos.ini",
			"launcher/eos.ini",
			"../launcher/eos.ini",
		});
	ApplyIni(cfg, cfg.iniPath);
}

} // namespace ac
