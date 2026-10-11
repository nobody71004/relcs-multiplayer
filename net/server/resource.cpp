// net/server/resource.cpp — mod/resource pipeline implementation.
#include "resource.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// pure helpers
// ---------------------------------------------------------------------------
static std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while(a < b && isspace((unsigned char)s[a])) a++;
	while(b > a && isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

static std::string Lower(std::string s)
{
	for(auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

bool ResourceManager::ParseManifest(const std::string& text, ResourceInfo& out)
{
	std::istringstream in(text);
	std::string line;
	bool gotName = false;
	while(std::getline(in, line)){
		if(!line.empty() && line.back() == '\r') line.pop_back();
		line = Trim(line);
		if(line.empty() || line[0] == '#') continue;
		size_t eq = line.find('=');
		if(eq == std::string::npos) continue;
		std::string key = Lower(Trim(line.substr(0, eq)));
		std::string val = Trim(line.substr(eq + 1));
		if(key == "name"){ out.name = val; gotName = true; }
		else if(key == "version") out.version = val;
		else if(key == "author") out.author = val;
		else if(key == "description") out.description = val;
		else if(key == "type") out.type = Lower(val);
		else if(key == "autoload") out.autoload = (val != "0" && Lower(val) != "false");
		else if(key == "motd") out.motd = val;
	}
	if(!gotName || out.name.empty()){
		out.error = "manifest missing name=";
		return false;
	}
	if(out.type != "internal" && out.type != "external"){
		out.error = "type must be internal or external";
		return false;
	}
	return true;
}

bool ResourceManager::ParseScript(const std::string& text, ResourceInfo& out)
{
	std::istringstream in(text);
	std::string line;
	ResourceHook* cur = nullptr;
	int lineNo = 0;
	while(std::getline(in, line)){
		lineNo++;
		if(!line.empty() && line.back() == '\r') line.pop_back();
		line = Trim(line);
		if(line.empty() || line[0] == '#') continue;
		if(line[0] == '@'){
			// @on <event> [arg]
			std::istringstream hs(line.substr(1));
			std::string on, ev, arg;
			hs >> on >> ev >> arg;
			if(Lower(on) != "on" || ev.empty()){
				out.error = "bad directive at line " + std::to_string(lineNo) + ": " + line;
				return false;
			}
			ev = Lower(ev);
			ResourceHook h;
			h.event = ev;
			if(ev == "interval"){
				h.interval = atof(arg.c_str());
				if(h.interval < 1.0){
					out.error = "interval needs >=1 seconds at line " + std::to_string(lineNo);
					return false;
				}
			}else if(ev == "cmd"){
				if(arg.empty()){
					out.error = "cmd hook needs a name at line " + std::to_string(lineNo);
					return false;
				}
				h.arg = Lower(arg);
			}else if(ev != "start" && ev != "join" && ev != "leave"){
				out.error = "unknown hook '" + ev + "' at line " + std::to_string(lineNo);
				return false;
			}
			out.hooks.push_back(h);
			cur = &out.hooks.back();
			continue;
		}
		if(!cur){
			out.error = "command before any @on directive at line " + std::to_string(lineNo);
			return false;
		}
		cur->lines.push_back(line);
	}
	return true;
}

std::string ResourceManager::Expand(const std::string& line, int id,
                                    const std::string& name, const std::string& args)
{
	std::string out;
	out.reserve(line.size() + 16);
	for(size_t i = 0; i < line.size(); ){
		if(line[i] == '%' && i + 1 < line.size()){
			size_t end = line.find('%', i + 1);
			if(end != std::string::npos){
				std::string tok = Lower(line.substr(i + 1, end - i - 1));
				if(tok == "id"){ out += std::to_string(id); i = end + 1; continue; }
				if(tok == "name"){ out += name; i = end + 1; continue; }
				if(tok == "args"){ out += args; i = end + 1; continue; }
			}
		}
		out += line[i++];
	}
	return out;
}

bool ResourceManager::ExternalAllowed(const std::string& line)
{
	// external (third-party) packs may only run these commands
	static const char* kAllowed[] = {
		"say", "broadcast", "settime", "setweather", "spawnprop", "give", nullptr
	};
	std::string cmd;
	size_t sp = line.find(' ');
	cmd = Lower(sp == std::string::npos ? line : line.substr(0, sp));
	for(int i = 0; kAllowed[i]; i++)
		if(cmd == kAllowed[i]) return true;
	return false;
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------
void ResourceManager::Configure(const std::string& dir, ExecFn exec, LogFn log)
{
	m_dir = dir;
	m_exec = exec;
	m_log = log;
}

void ResourceManager::LogLine(const std::string& s)
{
	if(m_log) m_log(s);
}

void ResourceManager::ResLog(ResourceInfo& res, const std::string& s)
{
	res.log.push_back(s);
	if(res.log.size() > 32) res.log.erase(res.log.begin());
}

bool ResourceManager::LoadDir(const std::string& path)
{
	fs::path root(path);
	ResourceInfo info;
	info.dir = root.filename().string();

	fs::path manifest = root / "resource.cfg";
	std::ifstream mf(manifest);
	if(!mf){
		info.error = "missing resource.cfg";
		info.valid = false;
		info.name = info.dir;
		m_res.push_back(info);
		return false;
	}
	std::stringstream ss;
	ss << mf.rdbuf();
	if(!ParseManifest(ss.str(), info)){
		info.valid = false;
		m_res.push_back(info);
		return false;
	}
	if(info.name.empty()) info.name = info.dir;

	bool allOk = true;
	for(auto& e : fs::directory_iterator(root)){
		if(!e.is_regular_file()) continue;
		if(e.path().extension() != ".rsc") continue;
		std::ifstream sf(e.path());
		std::stringstream sss;
		sss << sf.rdbuf();
		size_t hooksBefore = info.hooks.size();
		if(!ParseScript(sss.str(), info)){
			info.valid = false;
			allOk = false;
			break;
		}
		(void)hooksBefore;
	}
	info.valid = allOk;
	m_res.push_back(info);
	return allOk;
}

bool ResourceManager::Reload()
{
	m_res.clear();
	if(m_dir.empty()) return false;
	std::error_code ec;
	if(!fs::exists(m_dir, ec)){
		fs::create_directories(m_dir, ec);
		LogLine("[RES] created resources dir " + m_dir);
		return true;
	}
	int ok = 0, bad = 0;
	for(auto& e : fs::directory_iterator(m_dir, ec)){
		if(!e.is_directory(ec)) continue;
		std::string d = e.path().filename().string();
		if(d.empty() || d[0] == '.') continue;
		if(d == "removed") continue;   // uninstall graveyard, not a resource
		if(LoadDir(e.path().string())) ok++;
		else bad++;
	}
	LoadState();   // enabled flags persist across reloads (needs m_res filled)
	LogLine(("[RES] loaded " + std::to_string(ok) + " resources (" +
	         std::to_string(bad) + " with errors)").c_str());
	return true;
}

std::vector<ResourceInfo> ResourceManager::List() const
{
	return m_res;
}

const ResourceInfo* ResourceManager::Find(const std::string& name) const
{
	for(auto& r : m_res)
		if(r.dir == name || r.name == name) return &r;
	return nullptr;
}

// ---------------------------------------------------------------------------
// state persistence: <dir>/state.txt  lines "dir=0|1"
// ---------------------------------------------------------------------------
void ResourceManager::SaveState() const
{
	if(m_dir.empty()) return;
	std::ofstream f(fs::path(m_dir) / "state.txt", std::ios::trunc);
	for(auto& r : m_res) f << r.dir << "=" << (r.enabled ? 1 : 0) << "\n";
}

void ResourceManager::LoadState()
{
	std::ifstream f(fs::path(m_dir) / "state.txt");
	std::string line;
	std::vector<std::string> seen;
	while(std::getline(f, line)){
		if(!line.empty() && line.back() == '\r') line.pop_back();
		size_t eq = line.find('=');
		if(eq == std::string::npos) continue;
		std::string dir = Trim(line.substr(0, eq));
		bool on = Trim(line.substr(eq + 1)) != "0";
		seen.push_back(dir);
		for(auto& r : m_res)
			if(r.dir == dir) r.enabled = on;
	}
	// resources with no saved state (fresh installs) default to their autoload flag
	for(auto& r : m_res)
		if(std::find(seen.begin(), seen.end(), r.dir) == seen.end())
			r.enabled = r.autoload;
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------
void ResourceManager::RunHook(const ResourceInfo& res, const ResourceHook& h,
                              int respondTo, int playerId,
                              const std::string& playerName, const std::string& args)
{
	bool external = (res.type == "external");
	for(auto& raw : h.lines){
		std::string line = Expand(raw, playerId, playerName, args);
		if(external && !ExternalAllowed(line)){
			LogLine("[RES] " + res.dir + ": external sandbox blocked: " + line);
			continue;
		}
		if(m_exec) m_exec(line, respondTo);
	}
}

bool ResourceManager::RunEvent(const std::string& name, const std::string& event,
                               const std::string& arg, int respondTo,
                               int playerId, const std::string& playerName,
                               std::string& err)
{
	for(auto& r : m_res){
		if(r.dir != name && r.name != name) continue;
		if(!r.valid){ err = "resource has errors: " + r.error; return false; }
		if(!r.enabled){ err = "resource disabled"; return false; }
		int ran = 0;
		for(auto& h : r.hooks){
			if(h.event != event) continue;
			if(event == "cmd" && h.arg != Lower(arg)) continue;
			RunHook(r, h, respondTo, playerId, playerName, arg);
			ran++;
		}
		ResLog(const_cast<ResourceInfo&>(r),
		       "ran " + event + (arg.empty() ? "" : " " + arg) + " x" + std::to_string(ran));
		return true;
	}
	err = "no such resource: " + name;
	return false;
}

void ResourceManager::Tick(double now)
{
	for(auto& r : m_res){
		if(!r.valid || !r.enabled) continue;
		for(auto& h : r.hooks){
			if(h.event != "interval") continue;
			if(h.nextRun <= 0.0){ h.nextRun = now + h.interval; continue; }
			if(now >= h.nextRun){
				h.nextRun = now + h.interval;
				RunHook(r, h, -1, -1, "", "");
			}
		}
	}
}

void ResourceManager::OnPlayerJoin(int id, const std::string& name)
{
	for(auto& r : m_res){
		if(!r.valid || !r.enabled) continue;
		for(auto& h : r.hooks)
			if(h.event == "join")
				RunHook(r, h, -1, id, name, "");
	}
}

void ResourceManager::OnPlayerLeave(int id, const std::string& name)
{
	for(auto& r : m_res){
		if(!r.valid || !r.enabled) continue;
		for(auto& h : r.hooks)
			if(h.event == "leave")
				RunHook(r, h, -1, id, name, "");
	}
}

bool ResourceManager::OnPlayerCommand(const std::string& cmd, int playerId,
                                      const std::string& name)
{
	std::string cname = cmd, args;
	size_t sp = cmd.find(' ');
	if(sp != std::string::npos){
		cname = cmd.substr(0, sp);
		args = Trim(cmd.substr(sp + 1));
	}
	cname = Lower(cname);
	for(auto& r : m_res){
		if(!r.valid || !r.enabled) continue;
		for(auto& h : r.hooks){
			if(h.event != "cmd" || h.arg != cname) continue;
			RunHook(r, h, playerId, playerId, name, args);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// enable/disable + install/uninstall
// ---------------------------------------------------------------------------
bool ResourceManager::SetEnabled(const std::string& name, bool on, std::string& err)
{
	for(auto& r : m_res){
		if(r.dir != name && r.name != name) continue;
		r.enabled = on;
		SaveState();
		if(on && r.valid)
			for(auto& h : r.hooks)
				if(h.event == "start") RunHook(r, h, -1, -1, "", "");
		LogLine("[RES] " + r.dir + (on ? " enabled" : " disabled"));
		return true;
	}
	err = "no such resource: " + name;
	return false;
}

bool ResourceManager::Install(const std::string& srcDir, std::string& err)
{
	std::error_code ec;
	fs::path src(srcDir);
	if(!fs::is_directory(src, ec)){
		err = "not a directory: " + srcDir;
		return false;
	}
	ResourceInfo probe;
	probe.dir = src.filename().string();
	{
		std::ifstream mf(src / "resource.cfg");
		if(!mf){ err = "source has no resource.cfg"; return false; }
		std::stringstream ss;
		ss << mf.rdbuf();
		if(!ParseManifest(ss.str(), probe)){
			err = "bad manifest: " + probe.error;
			return false;
		}
	}
	fs::path dst = fs::path(m_dir) / probe.dir;
	if(fs::exists(dst, ec)){
		err = "already installed: " + probe.dir + " (uninstall first)";
		return false;
	}
	fs::copy(src, dst, fs::copy_options::recursive, ec);
	if(ec){
		err = "copy failed: " + ec.message();
		return false;
	}
	LogLine("[RES] installed " + probe.dir + " v" + probe.version +
	        " (" + probe.type + ")");
	Reload();
	std::string dummy;
	SetEnabled(probe.dir, true, dummy);
	return true;
}

bool ResourceManager::Uninstall(const std::string& name, std::string& err)
{
	for(auto& r : m_res){
		if(r.dir != name && r.name != name) continue;
		std::error_code ec;
		fs::path src = fs::path(m_dir) / r.dir;
		fs::path graveyard = fs::path(m_dir) / "removed";
		fs::create_directories(graveyard, ec);
		fs::path dst = graveyard / (r.dir + "-" + std::to_string(time(nullptr)));
		fs::rename(src, dst, ec);
		if(ec){
			err = "uninstall failed: " + ec.message();
			return false;
		}
		LogLine("[RES] uninstalled " + r.dir + " (moved to removed/)");
		Reload();
		return true;
	}
	err = "no such resource: " + name;
	return false;
}
