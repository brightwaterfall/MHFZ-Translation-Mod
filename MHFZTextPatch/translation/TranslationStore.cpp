#include "TranslationStore.h"
#include "../logging/Logger.h"
#include "../include/json.hpp"
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace {
	std::vector<TranslationEntry> g_entries;
	std::unordered_map<std::string, std::unordered_map<int, std::string>> g_byXpath;

	void AddEntry(const std::string& xpath, int index, const std::string& target) {
		if (xpath.empty() || index < 0 || target.empty())
			return;
		TranslationEntry e;
		e.xpath = xpath;
		e.index = index;
		e.target = target;
		g_byXpath[xpath][index] = target;
		g_entries.push_back(std::move(e));
	}

	int ParseIndex(const json& v) {
		if (v.is_number_integer())
			return v.get<int>();
		if (v.is_string()) {
			try {
				return std::stoi(v.get<std::string>());
			} catch (...) {
				return -1;
			}
		}
		return -1;
	}

	void ParseRowList(const std::string& xpath, const json& arr) {
		if (!arr.is_array())
			return;
		for (const auto& row : arr) {
			if (!row.is_object())
				continue;
			int index = -1;
			if (row.contains("index"))
				index = ParseIndex(row["index"]);
			else if (row.contains("i"))
				index = ParseIndex(row["i"]);
			std::string target;
			if (row.contains("target") && row["target"].is_string())
				target = row["target"].get<std::string>();
			else if (row.contains("t") && row["t"].is_string())
				target = row["t"].get<std::string>();
			AddEntry(xpath, index, target);
		}
	}

	bool ParseDocument(const json& doc) {
		g_entries.clear();
		g_byXpath.clear();

		if (doc.is_array()) {
			ParseRowList("", doc);
			return !g_entries.empty();
		}
		if (!doc.is_object())
			return false;

		// Mogapedia release: { "en": { "dat/items/name": [ {index,target}, ... ], ... } }
		json root = doc;
		if (doc.contains("en") && doc["en"].is_object())
			root = doc["en"];
		else if (doc.contains("translations") && doc["translations"].is_object())
			root = doc["translations"];

		if (root.contains("entries") && root["entries"].is_array()) {
			for (const auto& row : root["entries"]) {
				if (!row.is_object())
					continue;
				std::string xpath;
				if (row.contains("xpath") && row["xpath"].is_string())
					xpath = row["xpath"].get<std::string>();
				int index = row.contains("index") ? ParseIndex(row["index"]) : -1;
				std::string target;
				if (row.contains("target") && row["target"].is_string())
					target = row["target"].get<std::string>();
				AddEntry(xpath, index, target);
			}
			return !g_entries.empty();
		}

		if (root.contains("strings") && root["strings"].is_array()) {
			std::string xpath;
			if (root.contains("metadata") && root["metadata"].is_object() &&
				root["metadata"].contains("xpath") && root["metadata"]["xpath"].is_string()) {
				xpath = root["metadata"]["xpath"].get<std::string>();
			}
			ParseRowList(xpath, root["strings"]);
			return !g_entries.empty();
		}

		for (auto it = root.begin(); it != root.end(); ++it) {
			if (it.key() == "metadata" || it.key() == "meta")
				continue;
			if (it.value().is_array())
				ParseRowList(it.key(), it.value());
			else if (it.value().is_object() && it.value().contains("strings") &&
				it.value()["strings"].is_array()) {
				ParseRowList(it.key(), it.value()["strings"]);
			}
		}
		return !g_entries.empty();
	}
}

namespace TranslationStore {
	bool LoadFromFile(const std::string& path) {
		Clear();
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			Logger::Error("Translation payload not found: %s", path.c_str());
			return false;
		}
		std::stringstream buf;
		buf << in.rdbuf();
		const std::string raw = buf.str();
		if (raw.size() >= 2 && (uint8_t)raw[0] == 0x1F && (uint8_t)raw[1] == 0x8B) {
			Logger::Error(
				"Payload is gzip (%s). Run scripts\\fetch_payload.ps1 or gunzip to .json",
				path.c_str());
			return false;
		}
		try {
			const json doc = json::parse(raw);
			if (!ParseDocument(doc)) {
				Logger::Error("No translation rows parsed from %s", path.c_str());
				return false;
			}
			Logger::Info("Loaded %u translation rows (%u xpaths) from %s",
				static_cast<unsigned>(g_entries.size()),
				static_cast<unsigned>(g_byXpath.size()),
				path.c_str());
			return true;
		} catch (const std::exception& ex) {
			Logger::Error("JSON parse failed: %s", ex.what());
			return false;
		}
	}

	const std::vector<TranslationEntry>& Entries() { return g_entries; }
	const std::unordered_map<std::string, std::unordered_map<int, std::string>>& ByXpath() {
		return g_byXpath;
	}
	size_t Count() { return g_entries.size(); }
	void Clear() {
		g_entries.clear();
		g_byXpath.clear();
	}
}
