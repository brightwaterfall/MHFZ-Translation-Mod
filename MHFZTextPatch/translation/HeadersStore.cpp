#include "HeadersStore.h"
#include "../logging/Logger.h"
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace {
	std::unordered_map<std::string, HeadersStore::SectionConfig> g_sections;
	bool g_loaded = false;
	std::string g_gameVersion = "zz";

	int ResolveEntryCount(const json& v) {
		if (v.is_number_integer())
			return v.get<int>();
		if (v.is_object()) {
			if (v.contains(g_gameVersion) && v[g_gameVersion].is_number_integer())
				return v[g_gameVersion].get<int>();
			// tw/cn often share a Taiwan-private layout bucket
			if ((g_gameVersion == "tw" || g_gameVersion == "cn") &&
				v.contains("tw") && v["tw"].is_number_integer())
				return v["tw"].get<int>();
			if (v.contains("zz") && v["zz"].is_number_integer())
				return v["zz"].get<int>();
			for (auto it = v.begin(); it != v.end(); ++it) {
				if (it.value().is_number_integer())
					return it.value().get<int>();
			}
		}
		return 0;
	}

	uint32_t ParseHex(const json& v) {
		if (v.is_number_unsigned() || v.is_number_integer())
			return static_cast<uint32_t>(v.get<uint64_t>());
		if (v.is_string())
			return static_cast<uint32_t>(std::stoul(v.get<std::string>(), nullptr, 0));
		return 0;
	}

	void Walk(const json& node, const std::string& path, const std::string& fileKey) {
		if (!node.is_object())
			return;
		if (node.contains("begin_pointer")) {
			HeadersStore::SectionConfig cfg;
			cfg.xpath = path;
			cfg.fileKey = fileKey;
			cfg.beginPointer = ParseHex(node["begin_pointer"]);
			// Allow versioned begin_pointer: {"zz":"0x64","tw":"0x68"}
			if (node["begin_pointer"].is_object()) {
				const auto& bp = node["begin_pointer"];
				if (bp.contains(g_gameVersion))
					cfg.beginPointer = ParseHex(bp[g_gameVersion]);
				else if (bp.contains("tw") && (g_gameVersion == "tw" || g_gameVersion == "cn"))
					cfg.beginPointer = ParseHex(bp["tw"]);
				else if (bp.contains("zz"))
					cfg.beginPointer = ParseHex(bp["zz"]);
			}
			if (node.contains("entry_count"))
				cfg.entryCount = ResolveEntryCount(node["entry_count"]);
			if (node.contains("entry_size") && node["entry_size"].is_number_integer())
				cfg.entrySize = node["entry_size"].get<int>();
			if (node.contains("field_offset")) {
				const auto& fo = node["field_offset"];
				if (fo.is_number_integer()) {
					cfg.fieldOffset = fo.get<int>();
					cfg.fieldOffsets.push_back(cfg.fieldOffset);
				} else if (fo.is_array()) {
					for (const auto& x : fo) {
						if (x.is_number_integer())
							cfg.fieldOffsets.push_back(x.get<int>());
					}
					if (!cfg.fieldOffsets.empty())
						cfg.fieldOffset = cfg.fieldOffsets[0];
				}
			}
			if (node.contains("pointers_per_entry") && node["pointers_per_entry"].is_number_integer())
				cfg.pointersPerEntry = node["pointers_per_entry"].get<int>();
			cfg.nullPadding = node.value("null_padding", false);
			cfg.nullTerminated = node.value("null_terminated", false);
			cfg.groupedEntries = node.value("grouped_entries", false);
			cfg.literalBase = node.value("literal_base", false);
			cfg.questTable = node.value("quest_table", false);
			if (node.contains("max_sub_count") && node["max_sub_count"].is_number_integer())
				cfg.maxSubCount = node["max_sub_count"].get<int>();
			// Name-style tables (one string per slot) must not treat interior
			// nulls as {j} group separators — matches TextHandler intent when
			// max_sub_count is 1 even if null_padding was omitted.
			if (cfg.maxSubCount == 1)
				cfg.nullPadding = true;
			if (node.contains("next_field_pointer"))
				cfg.nextFieldPointer = ParseHex(node["next_field_pointer"]);
			if (node.contains("count_base_pointer"))
				cfg.countBasePointer = ParseHex(node["count_base_pointer"]);
			if (node.contains("count_offset"))
				cfg.countOffset = ParseHex(node["count_offset"]);
			if (node.contains("count_type") && node["count_type"].is_string())
				cfg.countType = node["count_type"].get<std::string>();
			if (node.contains("count_adjust") && node["count_adjust"].is_number_integer())
				cfg.countAdjust = node["count_adjust"].get<int>();
			if (node.contains("quest_text_offset"))
				cfg.questTextOffset = ParseHex(node["quest_text_offset"]);
			if (node.contains("text_pointers_count") && node["text_pointers_count"].is_number_integer())
				cfg.textPointersCount = node["text_pointers_count"].get<int>();
			cfg.valid = true;
			g_sections[path] = std::move(cfg);
			return;
		}
		for (auto it = node.begin(); it != node.end(); ++it) {
			if (it.key().empty() || it.key()[0] == '_')
				continue;
			const std::string child = path.empty() ? it.key() : (path + "/" + it.key());
			const std::string childFile = path.empty() ? it.key() : fileKey;
			Walk(it.value(), child, childFile);
		}
	}
}

namespace HeadersStore {
	void SetGameVersion(const std::string& version) {
		g_gameVersion = version.empty() ? "zz" : version;
		for (char& c : g_gameVersion) {
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		Logger::Info("Headers game version profile: %s", g_gameVersion.c_str());
	}

	const std::string& GameVersion() { return g_gameVersion; }

	bool LoadFromFile(const std::string& path) {
		g_sections.clear();
		g_loaded = false;
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			Logger::Warn("headers.json not found: %s", path.c_str());
			return false;
		}
		std::stringstream buf;
		buf << in.rdbuf();
		try {
			const json doc = json::parse(buf.str());
			Walk(doc, "", "");
			g_loaded = !g_sections.empty();
			Logger::Info("headers.json loaded: %u sections from %s",
				static_cast<unsigned>(g_sections.size()), path.c_str());
			return g_loaded;
		} catch (const std::exception& ex) {
			Logger::Error("headers.json parse failed: %s", ex.what());
			return false;
		}
	}

	bool Loaded() { return g_loaded; }

	SectionConfig Get(const std::string& xpath) {
		const auto it = g_sections.find(xpath);
		if (it == g_sections.end())
			return {};
		return it->second;
	}

	std::string FileKeyForXpath(const std::string& xpath) {
		const auto slash = xpath.find('/');
		if (slash == std::string::npos)
			return xpath;
		return xpath.substr(0, slash);
	}
}
