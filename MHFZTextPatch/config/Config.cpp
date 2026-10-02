#include "Config.h"
#include "../logging/Logger.h"
#include <Windows.h>
#include <fstream>
#include <sstream>

namespace {
	Config::Settings g_cfg;
	std::string IniPath() {
		char path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string p(path);
		const size_t slash = p.find_last_of("\\/");
		if (slash != std::string::npos)
			p = p.substr(0, slash + 1);
		return p + "mods\\mhfz_text_patch.ini";
	}
}

namespace Config {
	Settings& Get() { return g_cfg; }

	void Load() {
		const std::string path = IniPath();
		std::ifstream in(path);
		if (!in) {
			Save();
			return;
		}
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#' || line[0] == ';')
				continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string key = line.substr(0, eq);
			const std::string val = line.substr(eq + 1);
			if (key == "enabled") g_cfg.enabled = (val == "1" || val == "true");
			else if (key == "lang") g_cfg.lang = val;
			else if (key == "payload") g_cfg.payload = val;
			else if (key == "reapplyEachTick") g_cfg.reapplyEachTick = (val == "1" || val == "true");
			else if (key == "safeIndexApply") g_cfg.safeIndexApply = (val == "1" || val == "true");
			else if (key == "inPlaceOnly") g_cfg.inPlaceOnly = (val == "1" || val == "true");
			else if (key == "allowHeapReloc") g_cfg.allowHeapReloc = (val == "1" || val == "true");
			else if (key == "clientProfile") g_cfg.clientProfile = val;
			else if (key == "headersFile") g_cfg.headersFile = val;
		}
		Logger::Info("Config loaded: %s", path.c_str());
	}

	void Save() {
		const std::string path = IniPath();
		CreateDirectoryA((path.substr(0, path.find_last_of("\\/"))).c_str(), nullptr);
		std::ofstream out(path);
		if (!out) {
			Logger::Warn("Config save failed: %s", path.c_str());
			return;
		}
		out << "# MHFZ Text Patch\n";
		out << "enabled=" << (g_cfg.enabled ? 1 : 0) << "\n";
		out << "lang=" << g_cfg.lang << "\n";
		out << "payload=" << g_cfg.payload << "\n";
		if (g_cfg.payload.empty())
			out << "# empty payload = auto-pick translations-tw.json for tw/cn\n";
		out << "reapplyEachTick=" << (g_cfg.reapplyEachTick ? 1 : 0) << "\n";
		out << "safeIndexApply=" << (g_cfg.safeIndexApply ? 1 : 0) << "\n";
		out << "inPlaceOnly=" << (g_cfg.inPlaceOnly ? 1 : 0) << "\n";
		out << "# Dangerous: rewrite string pointers to VirtualAlloc heap\n";
		out << "allowHeapReloc=" << (g_cfg.allowHeapReloc ? 1 : 0) << "\n";
		out << "# Client structure: tw (Taiwan/CN), zz (JP), ko, cn\n";
		out << "clientProfile=" << g_cfg.clientProfile << "\n";
		out << "# Optional explicit headers filename in mods\\\n";
		out << "headersFile=" << g_cfg.headersFile << "\n";
	}
}
