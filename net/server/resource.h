// net/server/resource.h — mod/resource pipeline.
//
// A resource is a folder under the server's resources directory:
//
//   resources/<dir>/resource.cfg    manifest: key=value (name, version, author,
//                                   description, type=internal|external, autoload,
//                                   motd)
//   resources/<dir>/*.rsc           command scripts with @on hooks:
//
//       # comment
//       @on start                 runs when the resource is enabled / server start
//       @on join                  runs per joining player (%id% %name% expanded)
//       @on leave                 runs per leaving player
//       @on interval <seconds>    runs repeatedly while enabled
//       @on cmd <name>            runs when a player types /<name> [args]
//
//     script lines are ordinary server commands (say, broadcast, settime,
//     give <id> ..., spawnprop ...).  Placeholders: %id% %name% %args%.
//
// "external" resources are third-party packs: their scripts run sandboxed to a
// command whitelist (say/broadcast/settime/setweather/spawnprop/give).
// "internal" resources are owner-authored and get full command access.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct ResourceHook {
	std::string event;              // start | join | leave | interval | cmd
	std::string arg;                // interval seconds | command name | ""
	double interval = 0.0;          // parsed seconds (event == "interval")
	std::vector<std::string> lines;
	double nextRun = 0.0;           // runtime: next interval fire (server clock)
};

struct ResourceInfo {
	std::string dir;                // folder name (stable id)
	std::string name, version, author, description;
	std::string type = "internal";  // internal | external
	bool autoload = true;
	bool enabled = true;
	bool valid = false;
	std::string error;              // parse error when !valid
	std::string motd;
	std::vector<ResourceHook> hooks;
	std::vector<std::string> log;   // ring: recently executed lines / errors
};

class ResourceManager {
public:
	// exec(line, respondTo): run one server command line.
	// log(text): one server-log line.
	typedef std::function<void(const std::string&, int)> ExecFn;
	typedef std::function<void(const std::string&)> LogFn;

	void Configure(const std::string& dir, ExecFn exec, LogFn log);

	bool Reload();                                   // (re)scan the resources dir
	std::vector<ResourceInfo> List() const;
	const ResourceInfo* Find(const std::string& name) const;

	bool SetEnabled(const std::string& name, bool on, std::string& err);
	bool RunEvent(const std::string& name, const std::string& event,
	              const std::string& arg, int respondTo,
	              int playerId, const std::string& playerName, std::string& err);

	void Tick(double now);                           // fires interval hooks
	void OnPlayerJoin(int id, const std::string& name);
	void OnPlayerLeave(int id, const std::string& name);
	bool OnPlayerCommand(const std::string& cmd, int playerId,
	                     const std::string& playerName);   // "/cmd args"

	bool Install(const std::string& srcDir, std::string& err);  // copy pack in
	bool Uninstall(const std::string& name, std::string& err);  // move to removed/

	void SaveState() const;                          // persist enabled flags
	void LoadState();

	// pure helpers (unit-tested)
	static bool ParseManifest(const std::string& text, ResourceInfo& out);
	static bool ParseScript(const std::string& text, ResourceInfo& out);
	static std::string Expand(const std::string& line, int id,
	                          const std::string& name, const std::string& args);
	static bool ExternalAllowed(const std::string& line);

private:
	bool LoadDir(const std::string& path);
	void RunHook(const ResourceInfo& res, const ResourceHook& h, int respondTo,
	             int playerId, const std::string& playerName, const std::string& args);
	void LogLine(const std::string& s);
	void ResLog(ResourceInfo& res, const std::string& s);

	std::string m_dir;
	ExecFn m_exec;
	LogFn m_log;
	std::vector<ResourceInfo> m_res;
};
